#include "futu_trader/execution/kill_switch.hpp"

#include <cerrno>
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
  std::scoped_lock lock(mu_);
  if (!tripped_.load()) {
    reason_ = reason;  // keep the FIRST reason: later ones are usually consequences
    if (!persistPath_.empty()) {
      // Best effort: a failure to persist must not stop the in-memory halt.
      std::ofstream out(persistPath_, std::ios::trunc);
      out << reason << '\n';
      out.flush();
    }
  }
  tripped_.store(true, std::memory_order_release);
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
