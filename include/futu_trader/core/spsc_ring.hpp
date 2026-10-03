#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>
#include <vector>

namespace futu_trader {

/**
 * Bounded lock-free single-producer / single-consumer ring buffer.
 *
 * Exactly one thread may call tryPush and exactly one (other) thread may call tryPop; that is the
 * whole contract, and it is what makes the queue wait-free. Capacity is rounded up to a power of
 * two so indexing is a mask. Producer and consumer state live on separate cache lines, and each
 * side caches the other's index so the shared line is only touched when the cached value runs out
 * (the usual cure for cache-line ping-pong). T must be default-constructible and nothrow
 * copy/move-assignable; use small PODs. For best results make T a multiple of (and aligned to) a
 * cache line: otherwise adjacent slots share a line and the producer writing slot i contends with
 * the consumer reading slot i-1.
 *
 * It never allocates after construction and never blocks. A full ring returns false from tryPush:
 * the caller decides the policy (drop and count, conflate, or raise an alarm). Nothing is ever
 * silently overwritten.
 */
template <typename T>
class SpscRing {
  static_assert(std::is_default_constructible_v<T>, "ring slots are default-constructed up front");
  static_assert(std::is_nothrow_copy_assignable_v<T> && std::is_nothrow_move_assignable_v<T>,
                "tryPush copies and tryPop moves: neither may throw");

 public:
  explicit SpscRing(std::size_t minCapacity)
      : mask_(roundUpPow2(minCapacity < 2 ? 2 : minCapacity) - 1), buf_(mask_ + 1) {}

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  /** Producer side. Returns false if the ring is full (nothing is written). */
  bool tryPush(const T& value) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next = tail + 1;
    if (next - headCache_ > capacity()) {
      headCache_ = head_.load(std::memory_order_acquire);
      if (next - headCache_ > capacity()) {
        return false;
      }
    }
    buf_[tail & mask_] = value;
    tail_.store(next, std::memory_order_release);
    return true;
  }

  /** Consumer side. Returns false if the ring is empty (`out` is untouched). */
  bool tryPop(T& out) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tailCache_) {
      tailCache_ = tail_.load(std::memory_order_acquire);
      if (head == tailCache_) {
        return false;
      }
    }
    out = std::move(buf_[head & mask_]);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const { return mask_ + 1; }

  /**
   * Approximate occupancy, safe from any thread, always within [0, capacity]. The two indices are
   * read at slightly different instants, and while one of them is being read the other side can
   * produce and consume any number of items, so the raw difference can be stale or even exceed the
   * capacity: it is therefore clamped. It reads the OTHER side's cache line, so do not call it
   * per item on the hot path of either side.
   */
  std::size_t sizeApprox() const {
    // head first, then tail: tail never decreases, so tail >= head holds for these two loads and
    // the unsigned subtraction cannot wrap.
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t raw = tail - head;
    return raw > capacity() ? capacity() : raw;
  }

 private:
  static constexpr std::size_t roundUpPow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) {
      p <<= 1U;
    }
    return p;
  }

  static constexpr std::size_t kCacheLine = 64;

  const std::size_t mask_;
  std::vector<T> buf_;
  // Consumer-owned.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  alignas(kCacheLine) std::size_t tailCache_{0};
  // Producer-owned.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  alignas(kCacheLine) std::size_t headCache_{0};
};

}  // namespace futu_trader
