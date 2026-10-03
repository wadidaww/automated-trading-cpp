#include <gtest/gtest.h>

#include <cmath>
#include <set>

#include "futu_trader/backtest/metrics.hpp"
#include "futu_trader/backtest/synthetic.hpp"
#include "futu_trader/backtest/walk_forward.hpp"
#include "futu_trader/instrument/hk_rules.hpp"

using namespace futu_trader;
using namespace futu_trader::backtest;

// --- Metrics: reference values computed independently with Python's statistics module ----------

TEST(Metrics, KnownAnswerAgainstIndependentReference) {
  const std::vector<Money> equity{1'000'000, 1'010'000, 1'005'000, 1'025'000,
                                  1'025'000, 1'014'750, 1'030'000};
  const auto m = computeMetrics(equity, 252.0);
  EXPECT_EQ(m.periods, 6U);
  EXPECT_NEAR(m.totalReturn, 0.03, 1e-12);
  EXPECT_NEAR(m.annualizedReturn, 2.4606958935318217, 1e-9);
  EXPECT_NEAR(m.annualizedVol, 0.18737360816152895, 1e-12);
  EXPECT_NEAR(m.sharpe, 6.719676607929068, 1e-9);
  EXPECT_NEAR(m.sortino, 17.411434887473582, 1e-9);
  EXPECT_NEAR(m.maxDrawdown, 0.01, 1e-12);
  EXPECT_EQ(m.maxDrawdownPeriods, 1U);
  EXPECT_NEAR(m.calmar, 246.06958935318218, 1e-6);
  EXPECT_FALSE(m.ruined);
}

TEST(Metrics, DrawdownDepthAndDurationIncludeAnUnrecoveredTail) {
  // peak 120 at index 4, then 80 at the end: 1/3 drawdown, still open after 1 period.
  auto m = computeMetrics({100, 110, 90, 95, 120, 80}, 252.0);
  EXPECT_NEAR(m.maxDrawdown, 1.0 / 3.0, 1e-12);
  // 110 -> 90 -> 95 -> (recovered at 120): longest stretch below a peak is 2 periods (idx 2,3).
  EXPECT_EQ(m.maxDrawdownPeriods, 2U);
  m = computeMetrics({100, 99, 98, 97, 96}, 252.0);
  EXPECT_EQ(m.maxDrawdownPeriods, 4U);  // never recovers
}

TEST(Metrics, FewPeriodsAreFlaggedUnreliable) {
  // Review finding: +1% over a few hundred minutes annualises to an absurd number; the flag lets
  // reports say "do not read this" instead of presenting it as a result.
  EXPECT_FALSE(computeMetrics({1000, 1010, 1020, 1030}, 252.0 * 330.0).ratiosReliable);
  std::vector<Money> many;
  for (int i = 0; i < 400; ++i) {
    many.push_back(1'000'000 + (i * 10) + ((i % 3) * 7));
  }
  EXPECT_TRUE(computeMetrics(many, 252.0).ratiosReliable);
}

TEST(Metrics, FlatEquityHasZeroRatiosNotNaN) {
  const auto m = computeMetrics({1000, 1000, 1000, 1000}, 252.0);
  EXPECT_EQ(m.sharpe, 0.0);
  EXPECT_EQ(m.sortino, 0.0);
  EXPECT_EQ(m.calmar, 0.0);
  EXPECT_EQ(m.maxDrawdown, 0.0);
  EXPECT_FALSE(std::isnan(m.annualizedVol));
}

TEST(Metrics, RuinIsFlaggedInsteadOfProducingGarbageRatios) {
  const auto m = computeMetrics({1000, 500, 0, 100}, 252.0);
  EXPECT_TRUE(m.ruined);
  EXPECT_EQ(m.sharpe, 0.0);
}

TEST(Metrics, DegenerateInputs) {
  EXPECT_EQ(computeMetrics({}, 252.0).periods, 0U);
  EXPECT_EQ(computeMetrics({1000}, 252.0).periods, 0U);
  EXPECT_EQ(computeMetrics({1000, 1010}, 0.0).periods, 0U);
  EXPECT_TRUE(returnsFromEquity({1000, -5, 10}).empty());
}

TEST(Metrics, SharpeIsScaleInvariantAndAnnualisationScalesBySqrtPeriods) {
  const std::vector<double> r{0.01, -0.02, 0.015, 0.005, -0.003, 0.012};
  std::vector<double> scaled;
  for (double x : r) {
    scaled.push_back(x * 10.0);
  }
  EXPECT_NEAR(sharpeOf(r, 252.0), sharpeOf(scaled, 252.0), 1e-9);
  EXPECT_NEAR(sharpeOf(r, 252.0 * 4.0) / sharpeOf(r, 252.0), 2.0, 1e-9);
}

TEST(Resample, TakesTheLastValueAtOrBeforeEachBoundary) {
  const std::int64_t s = 1'000'000'000;
  std::vector<EquityPoint> pts{
      {0, 100}, {s / 2, 101}, {s + 1, 102}, {2 * s + 5, 103}, {2 * s + 6, 104}};
  const auto out = resampleEquity(pts, s);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_EQ(out[0], 101);  // last sample inside [0, 1s)
  EXPECT_EQ(out[1], 102);  // [1s, 2s)
  EXPECT_EQ(out[2], 104);  // [2s, 3s)
}

TEST(Resample, SkipsEmptyPeriodsInsteadOfInventingFlatOnes) {
  // Overnight/weekend gaps must not become thousands of zero-return periods (review finding).
  const std::int64_t s = 1'000'000'000;
  const auto out = resampleEquity({{0, 100}, {5 * s, 105}}, s);
  ASSERT_EQ(out.size(), 2U);
  EXPECT_EQ(out[0], 100);
  EXPECT_EQ(out[1], 105);  // the whole gap is one observation: exactly what happened
  EXPECT_TRUE(resampleEquity({}, s).empty());
  EXPECT_TRUE(resampleEquity({{0, 1}}, 0).empty());
}

TEST(Resample, ThreeTradingDaysGiveThreeDaysOfPeriodsNotThreeCalendarDays) {
  const std::int64_t minute = 60'000'000'000;
  const std::int64_t day = 24 * 60 * minute;
  std::vector<EquityPoint> pts;
  for (int d = 0; d < 3; ++d) {
    for (int m = 0; m < 331; ++m) {
      pts.push_back({(d * day) + (m * minute), 1'000'000 + m});
    }
  }
  const auto out = resampleEquity(pts, minute);
  EXPECT_EQ(out.size(), 3U * 331U);  // not ~4,300 minute-buckets of calendar time
}

TEST(TradeStats, KnownValues) {
  const auto s = computeTradeStats({300, -100, 200, -50, 0});
  EXPECT_EQ(s.count, 5U);
  EXPECT_EQ(s.wins, 2U);
  EXPECT_EQ(s.losses, 2U);
  EXPECT_NEAR(s.hitRate, 0.4, 1e-12);
  EXPECT_NEAR(s.avgWinMills, 250.0, 1e-12);
  EXPECT_NEAR(s.avgLossMills, -75.0, 1e-12);
  EXPECT_NEAR(s.profitFactor, 500.0 / 150.0, 1e-12);
  EXPECT_EQ(computeTradeStats({}).count, 0U);
  EXPECT_TRUE(std::isnan(computeTradeStats({10, 20}).profitFactor));  // undefined, not "worst"
}

// --- Bootstrap ----------------------------------------------------------------------------------

TEST(Bootstrap, IsDeterministicForASeed) {
  std::vector<double> r;
  Rng rng(3);
  for (int i = 0; i < 200; ++i) {
    r.push_back((static_cast<double>(rng.below(2001)) - 1000.0) / 100000.0);
  }
  const auto a = bootstrapSharpeCi(r, 252.0, 500, 7);
  const auto b = bootstrapSharpeCi(r, 252.0, 500, 7);
  EXPECT_EQ(a.lo, b.lo);
  EXPECT_EQ(a.hi, b.hi);
  EXPECT_LT(a.lo, a.hi);
}

TEST(Bootstrap, IntervalBracketsPointEstimateAndSeparatesEdgeFromNoise) {
  std::vector<double> edge;
  std::vector<double> noise;
  Rng rng(5);
  for (int i = 0; i < 400; ++i) {
    const double base = (static_cast<double>(rng.below(2001)) - 1000.0) / 100000.0;
    noise.push_back(base);
    edge.push_back(base + 0.004);  // a real per-period edge
  }
  const auto edgeCi = bootstrapSharpeCi(edge, 252.0, 1000, 1);
  const auto noiseCi = bootstrapSharpeCi(noise, 252.0, 1000, 1);
  EXPECT_GT(edgeCi.lo, 0.0);   // clearly positive
  EXPECT_LT(noiseCi.lo, 0.0);  // zero-mean noise: interval spans zero
  EXPECT_GT(noiseCi.hi, 0.0);
  const double point = sharpeOf(edge, 252.0);
  EXPECT_LT(edgeCi.lo, point);
  EXPECT_GT(edgeCi.hi, point);
}

TEST(Bootstrap, DegenerateInputsGiveAnEmptyInterval) {
  // An impossible interval must say so; {0, 0} would read as a real "no uncertainty" result.
  EXPECT_FALSE(bootstrapSharpeCi({0.01}, 252.0, 100, 1).valid);
  EXPECT_FALSE(bootstrapSharpeCi({0.01, 0.02}, 252.0, 0, 1).valid);
  EXPECT_FALSE(bootstrapSharpeCi({0.01, 0.02}, 252.0, 100, 1, 0.0).valid);
  EXPECT_TRUE(bootstrapSharpeCi({0.01, 0.02, 0.03}, 252.0, 100, 1).valid);
}

// --- Walk-forward -------------------------------------------------------------------------------

TEST(WalkForward, RollingFoldsAreEmbargoedAndNeverOverlapInTest) {
  const auto folds = walkForwardSplits(100, 40, 10, 2, false);
  ASSERT_EQ(folds.size(), 5U);
  EXPECT_EQ(folds[0].trainBegin, 0U);
  EXPECT_EQ(folds[0].trainEnd, 40U);
  EXPECT_EQ(folds[0].testBegin, 42U);
  EXPECT_EQ(folds[0].testEnd, 52U);
  EXPECT_EQ(folds[4].testEnd, 92U);
  for (std::size_t i = 0; i < folds.size(); ++i) {
    EXPECT_EQ(folds[i].testBegin - folds[i].trainEnd, 2U);    // the embargo
    EXPECT_EQ(folds[i].trainEnd - folds[i].trainBegin, 40U);  // rolling window length
    if (i > 0) {
      EXPECT_GE(folds[i].testBegin, folds[i - 1].testEnd);  // disjoint test windows
    }
  }
}

TEST(WalkForward, ExpandingKeepsTheStartFixedAndTrainNeverTouchesTest) {
  const auto folds = walkForwardSplits(100, 40, 10, 5, true);
  ASSERT_FALSE(folds.empty());
  for (const auto& f : folds) {
    EXPECT_EQ(f.trainBegin, 0U);
    EXPECT_LT(f.trainEnd, f.testBegin);
    EXPECT_LE(f.testEnd, 100U);
  }
  EXPECT_GT(folds.back().trainEnd - folds.back().trainBegin, 40U);  // it grew
}

TEST(WalkForward, DegenerateArgumentsGiveNoFolds) {
  EXPECT_TRUE(walkForwardSplits(100, 0, 10, 0, false).empty());
  EXPECT_TRUE(walkForwardSplits(100, 10, 0, 0, false).empty());
  EXPECT_TRUE(walkForwardSplits(30, 40, 10, 0, false).empty());  // not enough data
}

// --- Synthetic data -----------------------------------------------------------------------------

TEST(Synthetic, IsDeterministicAndSeedSensitive) {
  SyntheticConfig cfg;
  cfg.count = 500;
  const auto a = generateSyntheticQuotes(cfg);
  const auto b = generateSyntheticQuotes(cfg);
  EXPECT_EQ(a, b);
  cfg.seed = 43;
  EXPECT_NE(generateSyntheticQuotes(cfg), a);
}

TEST(Synthetic, QuotesAreValidOnTheTickGridAndTimeOrdered) {
  SyntheticConfig cfg;
  cfg.count = 5000;
  const auto quotes = generateSyntheticQuotes(cfg);
  ASSERT_EQ(quotes.size(), 5000U);
  std::set<Money> distinct;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto& q = quotes[i];
    ASSERT_TRUE(instrument::isTickAligned(q.bid)) << i;
    ASSERT_TRUE(instrument::isTickAligned(q.ask)) << i;
    ASSERT_GT(q.ask, q.bid) << i;
    ASSERT_TRUE(q.last == q.bid || q.last == q.ask) << i;
    if (i > 0) {
      ASSERT_GT(q.tsNs, quotes[i - 1].tsNs);
    }
    distinct.insert(q.bid);
  }
  EXPECT_GT(distinct.size(), 20U);  // it actually moves
}

TEST(Synthetic, RngGoldenValuesPinTheGeneratorAcrossPlatforms) {
  // If these change, every golden backtest changes with them. They must not depend on the
  // compiler or standard library. Values cross-checked against an independent Python
  // implementation of the same xorshift64* recurrence.
  Rng rng(42);
  EXPECT_EQ(rng.next(), 0x52A4C315EF2E339BULL);
  EXPECT_EQ(rng.below(1000), 467U);
}
