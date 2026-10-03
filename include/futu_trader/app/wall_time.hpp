#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace futu_trader::app {

/** Wall-clock time: only for things that outlive the process (log records, dates). Never for
 * latency. */
inline std::int64_t wallNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline constexpr std::int64_t kNsPerDay = 86'400LL * 1'000'000'000LL;
inline constexpr std::int64_t kHkOffsetNs = 8LL * 3600 * 1'000'000'000LL;  // UTC+8, no DST

/** Start of the Hong Kong day containing `wallNs`, as wall-clock ns. */
inline std::int64_t startOfHkDayNs(std::int64_t wallNs) {
  return ((wallNs + kHkOffsetNs) / kNsPerDay) * kNsPerDay - kHkOffsetNs;
}

/** "YYYY-MM-DD" in Hong Kong. */
inline std::string hkDate(std::int64_t wallNs) {
  const auto days = std::chrono::sys_days{std::chrono::days{(wallNs + kHkOffsetNs) / kNsPerDay}};
  const std::chrono::year_month_day ymd{days};
  std::array<char, 16> buf{};
  std::snprintf(buf.data(), buf.size(), "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
  return buf.data();
}

}  // namespace futu_trader::app
