#include "futu_trader/execution/kill_switch.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace futu_trader::execution {

KillSwitch::KillSwitch(std::string persistPath) : persistPath_(std::move(persistPath)) {
  if (persistPath_.empty()) {
    return;
  }
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(persistPath_, ec);
  const bool absent = ec == std::errc::no_such_file_or_directory ||
                      (!ec && status.type() == std::filesystem::file_type::not_found);
  if (absent) {
    return;
  }
  // Present, or unreadable: either way we cannot prove it is safe to trade.
  std::ostringstream content;
  std::ifstream in(persistPath_);
  content << in.rdbuf();
  reason_ = "persisted halt from a previous run: " + content.str();
  tripped_.store(true, std::memory_order_release);
}

void KillSwitch::trip(const std::string& reason) {
  bool first = false;
  {
    std::scoped_lock lock(mu_);
    if (!tripped_.load()) {
      reason_ = reason;  // keep the FIRST reason: later ones are usually consequences
      first = true;
    }
    tripped_.store(true, std::memory_order_release);  // the in-memory halt never waits for the disk
  }
  if (!first || persistPath_.empty()) {
    return;
  }
  // Persist OUTSIDE the lock, durably: written, fsync'd and the directory entry synced, so a power
  // loss right after a halt cannot let a restart trade. If it still fails the halt stands in memory
  // and the failure is shouted, because a restart would then not be blocked.
  bool ok = false;
  const int fd =
      ::open(persistPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd >= 0) {
    const std::string line = reason + "\n";
    ok = ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()) &&
         ::fsync(fd) == 0;
    ::close(fd);
    const std::string dir = std::filesystem::path(persistPath_).parent_path().string();
    const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
      static_cast<void>(::fsync(dfd));
      ::close(dfd);
    }
  }
  if (!ok) {
    persistFailed_.store(true, std::memory_order_release);
    std::fprintf(stderr,
                 "CRITICAL: kill switch tripped (%s) but could NOT be persisted to %s: a restart "
                 "will not be blocked. Do not restart until the cause is understood.\n",
                 reason.c_str(), persistPath_.c_str());
  }
}

std::string KillSwitch::reason() const {
  std::scoped_lock lock(mu_);
  return reason_;
}

bool KillSwitch::reset(const std::string& operatorName) {
  if (operatorName.empty()) {
    return false;
  }
  std::scoped_lock lock(mu_);
  resetBy_ = operatorName;
  reason_.clear();
  if (!persistPath_.empty()) {
    std::error_code ec;
    std::filesystem::remove(persistPath_, ec);
  }
  tripped_.store(false, std::memory_order_release);
  return true;
}

std::string KillSwitch::lastResetBy() const {
  std::scoped_lock lock(mu_);
  return resetBy_;
}

bool KillSwitch::checkFlagFile(const std::string& path) {
  std::error_code ec;
  // symlink_status does not follow links, so a dangling symlink still reads as "present".
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      (!ec && status.type() == std::filesystem::file_type::not_found)) {
    return false;
  }
  if (ec) {
    trip("cannot examine kill flag " + path + ": " + ec.message());
    return true;
  }
  trip("kill flag file present: " + path);
  return true;
}

}  // namespace futu_trader::execution
