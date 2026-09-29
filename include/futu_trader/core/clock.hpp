#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace futu_trader {

/** Time source injected everywhere that needs "now", so tests and backtests control time. */
class Clock {
 public:
  virtual ~Clock() = default;
  /** Monotonic nanoseconds. Only differences are meaningful. */
  virtual std::int64_t nowNs() const = 0;
};

class SteadyClock final : public Clock {
 public:
  std::int64_t nowNs() const override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

/** Manually advanced clock for deterministic tests. */
class ManualClock final : public Clock {
 public:
  std::int64_t nowNs() const override { return now_.load(); }
  void advanceMs(std::int64_t ms) { now_ += ms * 1'000'000; }
  void setNs(std::int64_t ns) { now_ = ns; }

 private:
  std::atomic<std::int64_t> now_{0};
};

}  // namespace futu_trader
