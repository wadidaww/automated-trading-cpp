#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace futu_trader::infra {

/**
 * Fixed-size log-linear latency histogram (HdrHistogram-style: constant relative error, no
 * allocation, O(1) record). Values are nanoseconds (or any non-negative integer unit).
 *
 * Buckets: values below 16 are exact; above that each power of two is split into 16 linear
 * sub-buckets, so any recorded value is within ~6% of its bucket's reported value. The covered
 * range is [0, 2^40) ~ 18 minutes of nanoseconds; larger values land in the last bucket.
 *
 * record() is safe to call from many threads and read concurrently (relaxed atomics: counts are
 * eventually consistent, never torn). Percentiles are upper bounds of the containing bucket, so
 * they never understate a latency.
 */
class LatencyHistogram {
 public:
  static constexpr std::size_t kSubBuckets = 16;
  static constexpr std::size_t kExponents = 36;  // 2^4 .. 2^40
  static constexpr std::size_t kBuckets = kSubBuckets + (kExponents * kSubBuckets);

  void record(std::uint64_t value) {
    counts_[indexOf(value)].fetch_add(1, std::memory_order_relaxed);
    total_.fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(value, std::memory_order_relaxed);
    std::uint64_t seenMax = max_.load(std::memory_order_relaxed);
    while (value > seenMax &&
           !max_.compare_exchange_weak(seenMax, value, std::memory_order_relaxed)) {
    }
  }

  std::uint64_t count() const { return total_.load(std::memory_order_relaxed); }
  std::uint64_t max() const { return max_.load(std::memory_order_relaxed); }
  double mean() const {
    const std::uint64_t n = count();
    return n == 0
               ? 0.0
               : static_cast<double>(sum_.load(std::memory_order_relaxed)) / static_cast<double>(n);
  }

  /** Smallest bucket upper bound covering at least `q` (0..1) of the samples; 0 if empty. */
  std::uint64_t percentile(double q) const;

  /** Cumulative count of samples <= `bound`'s bucket (for Prometheus-style exposition). */
  std::uint64_t countAtOrBelow(std::uint64_t bound) const;

  /** Adds another histogram's samples into this one. */
  void merge(const LatencyHistogram& other);
  void reset();

  /** Upper bound (inclusive) of bucket `index`. */
  static std::uint64_t upperBound(std::size_t index);
  static std::size_t indexOf(std::uint64_t value);

 private:
  std::array<std::atomic<std::uint64_t>, kBuckets> counts_{};
  std::atomic<std::uint64_t> total_{0};
  std::atomic<std::uint64_t> sum_{0};
  std::atomic<std::uint64_t> max_{0};
};

}  // namespace futu_trader::infra
