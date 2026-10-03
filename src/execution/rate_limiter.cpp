#include "futu_trader/execution/rate_limiter.hpp"

namespace futu_trader::execution {

void RateLimiter::expire(std::int64_t now) {
  const std::int64_t cutoff = now - (config_.windowMs * 1'000'000);
  while (!stamps_.empty() && stamps_.front() <= cutoff) {
    stamps_.pop_front();
  }
}

bool RateLimiter::tryAcquire(RequestKind kind) {
  std::scoped_lock lock(mu_);
  const std::int64_t now = clock_.nowNs();
  expire(now);
  const std::size_t reserve = kind == RequestKind::kCancel ? 0 : config_.reservedForCancels;
  const std::size_t allowed = config_.maxPerWindow > reserve ? config_.maxPerWindow - reserve : 0;
  if (stamps_.size() >= allowed) {
    return false;
  }
  stamps_.push_back(now);
  return true;
}

std::size_t RateLimiter::used() {
  std::scoped_lock lock(mu_);
  expire(clock_.nowNs());
  return stamps_.size();
}

double RateLimiter::utilization() {
  std::scoped_lock lock(mu_);
  expire(clock_.nowNs());
  if (config_.maxPerWindow == 0) {
    return 1.0;
  }
  return static_cast<double>(stamps_.size()) / static_cast<double>(config_.maxPerWindow);
}

}  // namespace futu_trader::execution
