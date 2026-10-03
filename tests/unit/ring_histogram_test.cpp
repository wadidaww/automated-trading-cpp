#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include "futu_trader/core/spsc_ring.hpp"
#include "futu_trader/infra/histogram.hpp"

using namespace futu_trader;
using namespace futu_trader::infra;

// --- SPSC ring ----------------------------------------------------------------------------------

TEST(SpscRing, CapacityRoundsUpToAPowerOfTwo) {
  EXPECT_EQ(SpscRing<int>(1).capacity(), 2U);
  EXPECT_EQ(SpscRing<int>(5).capacity(), 8U);
  EXPECT_EQ(SpscRing<int>(8).capacity(), 8U);
  EXPECT_EQ(SpscRing<int>(1000).capacity(), 1024U);
}

TEST(SpscRing, FifoOrderAndEmptyFullBoundaries) {
  SpscRing<int> ring(4);
  int out = -1;
  EXPECT_FALSE(ring.tryPop(out));
  EXPECT_EQ(out, -1);  // untouched when empty
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(ring.tryPush(i));
  }
  EXPECT_FALSE(ring.tryPush(99));  // full: refused, nothing overwritten
  EXPECT_EQ(ring.sizeApprox(), 4U);
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(ring.tryPop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_FALSE(ring.tryPop(out));
  EXPECT_EQ(ring.sizeApprox(), 0U);
}

TEST(SpscRing, SurvivesIndexWraparoundManyTimes) {
  SpscRing<std::uint64_t> ring(8);
  std::uint64_t expected = 0;
  std::uint64_t next = 0;
  for (int round = 0; round < 10'000; ++round) {
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(ring.tryPush(next++));
    }
    std::uint64_t out = 0;
    for (int i = 0; i < 5; ++i) {
      ASSERT_TRUE(ring.tryPop(out));
      ASSERT_EQ(out, expected++);
    }
  }
}

TEST(SpscRing, SizeApproxNeverWrapsOrExceedsCapacityEvenFromAnotherThread) {
  // Review finding: loading tail before head let a concurrent consumer make head > tail and the
  // unsigned subtraction wrap to ~2^64.
  SpscRing<std::uint64_t> ring(64);
  std::atomic<bool> stop{false};
  std::thread producer([&] {
    std::uint64_t i = 0;
    while (!stop.load()) {
      ring.tryPush(i++);
    }
  });
  std::thread consumer([&] {
    std::uint64_t v = 0;
    while (!stop.load()) {
      ring.tryPop(v);
    }
  });
  std::uint64_t worst = 0;
  for (int i = 0; i < 2'000'000; ++i) {
    worst = std::max<std::uint64_t>(worst, ring.sizeApprox());
  }
  stop = true;
  producer.join();
  consumer.join();
  EXPECT_LE(worst, ring.capacity());
}

TEST(SpscRing, WorksForPodStructs) {
  struct Tick {
    std::int64_t a;
    std::int64_t b;
    char symbol[16];
  };
  SpscRing<Tick> ring(4);
  Tick in{1, 2, "00700"};
  ASSERT_TRUE(ring.tryPush(in));
  Tick out{};
  ASSERT_TRUE(ring.tryPop(out));
  EXPECT_EQ(out.a, 1);
  EXPECT_STREQ(out.symbol, "00700");
}

TEST(SpscRing, TwoThreadsDeliverEveryItemInOrderWithoutLossOrDuplication) {
  // Run under TSan in CI: any data race on the indices or slots is reported.
  constexpr std::uint64_t kCount = 400'000;
  SpscRing<std::uint64_t> ring(64);  // small, so it is constantly full/empty
  std::thread producer([&] {
    for (std::uint64_t i = 1; i <= kCount; ++i) {
      while (!ring.tryPush(i)) {
        std::this_thread::yield();
      }
    }
  });
  std::uint64_t expected = 1;
  std::uint64_t sum = 0;
  while (expected <= kCount) {
    std::uint64_t value = 0;
    if (ring.tryPop(value)) {
      ASSERT_EQ(value, expected) << "out of order, lost or duplicated item";
      sum += value;
      ++expected;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  EXPECT_EQ(sum, kCount * (kCount + 1) / 2);
  std::uint64_t leftover = 0;
  EXPECT_FALSE(ring.tryPop(leftover));
}

// --- Histogram ----------------------------------------------------------------------------------

TEST(Histogram, SmallValuesAreExact) {
  LatencyHistogram h;
  for (std::uint64_t v = 0; v < 16; ++v) {
    EXPECT_EQ(LatencyHistogram::upperBound(LatencyHistogram::indexOf(v)), v);
  }
}

TEST(Histogram, EveryValueIsWithinAboutSixPercentOfItsBucket) {
  std::mt19937_64 rng(5);
  for (int i = 0; i < 100'000; ++i) {
    const std::uint64_t v = rng() >> (rng() % 40 + 20);  // values across ~40 powers of two
    const std::size_t idx = LatencyHistogram::indexOf(v);
    const std::uint64_t upper = LatencyHistogram::upperBound(idx);
    ASSERT_GE(upper, v) << v;  // a bucket never understates what it holds
    if (v >= 16 && idx < LatencyHistogram::kBuckets - 1) {  // covered range: tight resolution
      ASSERT_LE(static_cast<double>(upper), static_cast<double>(v) * 1.0626) << v;
    }
  }
}

TEST(Histogram, ValuesBeyondTheRangeLandInAnOpenEndedBucketAndStillReportTheTrueMax) {
  LatencyHistogram h;
  const std::uint64_t huge = 20'000'000'000'000ULL;  // ~5.5 hours in ns, past the 2^40 range
  h.record(huge);
  EXPECT_EQ(LatencyHistogram::indexOf(huge), LatencyHistogram::kBuckets - 1);
  EXPECT_EQ(LatencyHistogram::upperBound(LatencyHistogram::kBuckets - 1), UINT64_MAX);
  EXPECT_EQ(h.max(), huge);
  EXPECT_EQ(h.percentile(0.5), huge);  // clamped to the observed maximum, not understated
}

TEST(Histogram, BucketIndicesAreMonotonicAndInRange) {
  std::size_t previous = 0;
  for (std::uint64_t v = 0; v < 5'000'000; v += 7) {
    const std::size_t idx = LatencyHistogram::indexOf(v);
    ASSERT_GE(idx, previous);
    ASSERT_LT(idx, LatencyHistogram::kBuckets);
    previous = idx;
  }
  EXPECT_EQ(LatencyHistogram::indexOf(UINT64_MAX), LatencyHistogram::kBuckets - 1);
}

TEST(Histogram, PercentilesOfAKnownDistribution) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 1000; ++v) {
    h.record(v);
  }
  EXPECT_EQ(h.count(), 1000U);
  EXPECT_EQ(h.max(), 1000U);
  EXPECT_NEAR(h.mean(), 500.5, 1e-9);
  // Percentiles are bucket upper bounds: >= the true value, within bucket resolution.
  const auto p50 = h.percentile(0.50);
  const auto p99 = h.percentile(0.99);
  EXPECT_GE(p50, 500U);
  EXPECT_LE(p50, 540U);
  EXPECT_GE(p99, 990U);
  EXPECT_LE(p99, 1000U);  // clamped to the observed maximum, never above it
  EXPECT_EQ(h.percentile(1.0), 1000U);
  EXPECT_EQ(LatencyHistogram().percentile(0.5), 0U);
}

TEST(Histogram, OutliersDoNotHideInTheMean) {
  LatencyHistogram h;
  for (int i = 0; i < 9990; ++i) {
    h.record(1000);
  }
  for (int i = 0; i < 10; ++i) {
    h.record(5'000'000);  // 0.1% are 5 ms stalls
  }
  EXPECT_LE(h.percentile(0.99), 1100U);         // the 99th percentile looks healthy...
  EXPECT_GE(h.percentile(0.9995), 4'000'000U);  // ...but p99.95 exposes the stalls
  EXPECT_EQ(h.max(), 5'000'000U);
}

TEST(Histogram, CumulativeCountsMatchForExposition) {
  LatencyHistogram h;
  for (std::uint64_t v : {5ULL, 10ULL, 100ULL, 1000ULL, 10'000ULL}) {
    h.record(v);
  }
  EXPECT_EQ(h.countAtOrBelow(7), 1U);
  EXPECT_EQ(h.countAtOrBelow(120), 3U);
  EXPECT_EQ(h.countAtOrBelow(1'000'000), 5U);
}

TEST(Histogram, MergeAddsCountsAndKeepsTheLargerMax) {
  LatencyHistogram a;
  LatencyHistogram b;
  a.record(100);
  b.record(300);
  b.record(900);
  a.merge(b);
  EXPECT_EQ(a.count(), 3U);
  EXPECT_EQ(a.max(), 900U);
  a.reset();
  EXPECT_EQ(a.count(), 0U);
  EXPECT_EQ(a.max(), 0U);
}

TEST(Histogram, ConcurrentRecordingLosesNothing) {
  LatencyHistogram h;
  constexpr int kThreads = 4;
  constexpr int kEach = 50'000;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&h, t] {
      for (int i = 0; i < kEach; ++i) {
        h.record(static_cast<std::uint64_t>(100 + t));
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  EXPECT_EQ(h.count(), static_cast<std::uint64_t>(kThreads) * kEach);
  EXPECT_EQ(h.max(), 103U);
}
