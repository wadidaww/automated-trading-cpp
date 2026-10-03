#include "futu_trader/app/promotion.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

#include "futu_trader/oms/live_gate.hpp"

namespace futu_trader::app {

Result<bool> recordPromotionDay(const std::string& path, const std::string& date, bool clean) {
  std::vector<oms::PromotionEntry> entries;
  {
    std::ifstream in(path, std::ios::binary);
    if (in) {
      std::ostringstream text;
      text << in.rdbuf();
      auto parsed = oms::parsePromotionLog(text.str());
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

  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
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
  return true;
}

}  // namespace futu_trader::app
