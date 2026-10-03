#include "futu_trader/app/promotion.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "futu_trader/oms/live_gate.hpp"

namespace futu_trader::app {

Result<bool> recordPromotionDay(const std::string& path, const std::string& date, bool clean) {
  std::vector<oms::PromotionEntry> entries;
  {
    // The existing log is evidence the gate will trust, so it gets the gate's own checks: a regular
    // file we own that others cannot write. Re-writing someone else's file as ours would launder
    // it.
    const int rfd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (rfd < 0 && errno != ENOENT) {
      return Error{ErrorCode::kInvalidArg,
                   "cannot open promotion log (a symlink?), left untouched: " + path};
    }
    if (rfd >= 0) {
      struct stat info {};
      std::string text;
      bool trusted = ::fstat(rfd, &info) == 0 && S_ISREG(info.st_mode) &&
                     info.st_uid == ::geteuid() && (info.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
                     info.st_size >= 0 &&
                     static_cast<std::size_t>(info.st_size) <= oms::kMaxPromotionLogBytes;
      if (trusted) {
        std::array<char, 4096> buf{};
        ssize_t n = 0;
        while ((n = ::read(rfd, buf.data(), buf.size())) > 0 &&
               text.size() <= oms::kMaxPromotionLogBytes) {
          text.append(buf.data(), static_cast<std::size_t>(n));
        }
        trusted = text.size() <= oms::kMaxPromotionLogBytes;
      }
      ::close(rfd);
      if (!trusted) {
        return Error{
            ErrorCode::kInvalidArg,
            "promotion log is not a private regular file of ours, left untouched: " + path};
      }
      auto parsed = oms::parsePromotionLog(text);
      if (!parsed) {
        return Error{ErrorCode::kInvalidArg,
                     "promotion log is invalid, left untouched: " + parsed.error().message};
      }
      entries = std::move(parsed.value());
    }
  }

  if (!entries.empty() && date < entries.back().date) {  // ISO dates order lexicographically
    return Error{
        ErrorCode::kInvalidArg,
        "refusing to record " + date + ": earlier than the last entry " + entries.back().date};
  }
  const oms::PromotionEntry today{date, clean};
  if (!entries.empty() && entries.back().date == date) {
    entries.back().clean = entries.back().clean && clean;  // dirty is sticky within a day
  } else {
    entries.push_back(today);
  }
  std::string body;
  for (const auto& entry : entries) {
    body += oms::formatPromotionEntry(entry);
    if (body.empty() || body.back() != '\n') {
      body += '\n';
    }
  }
  // A log that would no longer parse (date out of order, bad date) is refused before writing.
  if (const auto check = oms::parsePromotionLog(body); !check) {
    return Error{ErrorCode::kInvalidArg,
                 "refusing to write an invalid promotion log: " + check.error().message};
  }

  // O_EXCL | O_NOFOLLOW: never follow or reuse a pre-planted name; a pid suffix keeps it unique.
  const std::string tmp = path + ".tmp." + std::to_string(::getpid());
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    return Error{ErrorCode::kInvalidArg, "cannot write " + tmp + ": " + std::strerror(errno)};
  }
  std::size_t done = 0;
  bool ok = true;
  while (done < body.size() && ok) {
    const ssize_t n = ::write(fd, body.data() + done, body.size() - done);
    if (n < 0 && errno != EINTR) {
      ok = false;
    } else if (n > 0) {
      done += static_cast<std::size_t>(n);
    }
  }
  ok = ok && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return Error{ErrorCode::kInvalidArg, "cannot update promotion log " + path};
  }
  // Make the rename itself durable.
  const std::string dir = std::filesystem::path(path).parent_path().string();
  const int dfd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    static_cast<void>(::fsync(dfd));
    ::close(dfd);
  }
  return true;
}

}  // namespace futu_trader::app
