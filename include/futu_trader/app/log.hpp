#pragma once

#include <array>
#include <chrono>
#include <ctime>
#include <functional>
#include <iostream>
#include <string>

namespace futu_trader::app {

using Log = std::function<void(const std::string&)>;

/** One line per call to stderr, prefixed with a UTC timestamp (journald adds its own as well). */
inline Log defaultLog() {
  return [](const std::string& line) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::array<char, 32> stamp{};
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::strftime(stamp.data(), stamp.size(), "%Y-%m-%dT%H:%M:%SZ", &tm);
    std::cerr << stamp.data() << " " << line << "\n";
  };
}

}  // namespace futu_trader::app
