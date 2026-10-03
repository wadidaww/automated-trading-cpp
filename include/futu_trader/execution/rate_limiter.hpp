#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "futu_trader/core/clock.hpp"

namespace futu_trader::execution {

struct RateLimitConfig {
  std::size_t maxPerWindow{12};  // set below OpenD's ~15/30 s so we never hit the hard limit
  std::int64_t windowMs{30'000};
  std::size_t reservedForCancels{3};  // budget only cancels may use (kill switch must always work)
};

enum class RequestKind : std::uint8_t { kNewOrder, kModify, kCancel };

/** Sliding-window limiter for order-writing requests. Thread-safe; time comes from a Clock. */
class RateLimiter {
 public:
  RateLimiter(RateLimitConfig config, const Clock& clock) : config_(config), clock_(clock) {}

  /** Consumes one slot if allowed. New orders and modifies cannot touch the cancel reserve. */
  bool tryAcquire(RequestKind kind);
  std::size_t used();
  /** Fraction of the window budget currently used, for alerting (> 0.8 should page). */
  double utilization();

 private:
  void expire(std::int64_t now);

  RateLimitConfig config_;
  const Clock& clock_;
  std::mutex mu_;
  std::deque<std::int64_t> stamps_;
};

}  // namespace futu_trader::execution
