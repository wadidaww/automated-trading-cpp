#include "futu_trader/infra/histogram.hpp"

#include <algorithm>

namespace futu_trader::infra {

namespace {

constexpr unsigned kSubBits = 4;  // log2(kSubBuckets)

unsigned floorLog2(std::uint64_t v) {
  unsigned r = 0;
  while (v >>= 1U) {
    ++r;
  }
  return r;
}

}  // namespace

std::size_t LatencyHistogram::indexOf(std::uint64_t value) {
  if (value < kSubBuckets) {
    return static_cast<std::size_t>(value);
  }
  const unsigned exponent = floorLog2(value);  // >= 4
  const unsigned relative = exponent - kSubBits;
  if (relative >= kExponents) {
    return kBuckets - 1;
  }
  const std::uint64_t sub = (value >> relative) - kSubBuckets;  // 0..15
  return kSubBuckets + (static_cast<std::size_t>(relative) * kSubBuckets) +
         static_cast<std::size_t>(sub);
}

std::uint64_t LatencyHistogram::upperBound(std::size_t index) {
  if (index < kSubBuckets) {
    return index;
  }
  if (index >= kBuckets - 1) {
    return UINT64_MAX;  // the overflow bucket is open-ended: it must never understate
  }
  const std::size_t offset = index - kSubBuckets;
  const std::size_t relative = offset / kSubBuckets;
  const std::size_t sub = offset % kSubBuckets;
  const std::uint64_t lower = (static_cast<std::uint64_t>(kSubBuckets) + sub) << relative;
  return lower + ((std::uint64_t{1} << relative) - 1);
}

std::uint64_t LatencyHistogram::percentile(double q) const {
  const std::uint64_t n = count();
  if (n == 0) {
    return 0;
  }
  q = std::clamp(q, 0.0, 1.0);
  // Rank of the sample we need: ceil(q * n), at least 1.
  auto rank = static_cast<std::uint64_t>(q * static_cast<double>(n));
  if (static_cast<double>(rank) < q * static_cast<double>(n)) {
    ++rank;
  }
  rank = std::max<std::uint64_t>(rank, 1);
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < kBuckets; ++i) {
    seen += counts_[i].load(std::memory_order_relaxed);
    if (seen >= rank) {
      // Never report above the true maximum: the top bucket's bound can overshoot it.
      return std::min(upperBound(i), max());
    }
  }
  return max();
}

std::uint64_t LatencyHistogram::countAtOrBelow(std::uint64_t bound) const {
  std::uint64_t seen = 0;
  const std::size_t last = indexOf(bound);
  for (std::size_t i = 0; i <= last && i < kBuckets; ++i) {
    seen += counts_[i].load(std::memory_order_relaxed);
  }
  return seen;
}

void LatencyHistogram::merge(const LatencyHistogram& other) {
  for (std::size_t i = 0; i < kBuckets; ++i) {
    counts_[i].fetch_add(other.counts_[i].load(std::memory_order_relaxed),
                         std::memory_order_relaxed);
  }
  total_.fetch_add(other.total_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  sum_.fetch_add(other.sum_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  const std::uint64_t otherMax = other.max_.load(std::memory_order_relaxed);
  std::uint64_t seenMax = max_.load(std::memory_order_relaxed);
  while (otherMax > seenMax &&
         !max_.compare_exchange_weak(seenMax, otherMax, std::memory_order_relaxed)) {
  }
}

void LatencyHistogram::reset() {
  for (auto& c : counts_) {
    c.store(0, std::memory_order_relaxed);
  }
  total_.store(0, std::memory_order_relaxed);
  sum_.store(0, std::memory_order_relaxed);
  max_.store(0, std::memory_order_relaxed);
}

}  // namespace futu_trader::infra
