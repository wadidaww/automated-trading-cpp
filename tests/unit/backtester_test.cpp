#include "futu_trader/evaluation/backtester.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "futu_trader/model/signal_model.hpp"

using namespace futu_trader;

namespace {

// Always long, regardless of features.
class AlwaysBuy : public ISignalModel {
 public:
  Signal predict(const FeatureVector&) const override { return {SignalAction::kBuy, 1.0}; }
};

// Cheats: buys iff the *current* input return is positive. It sees the same-bar return, so if the
// backtester feeds it the return it then earns, it looks like a money machine.
class Peeker : public ISignalModel {
 public:
  Signal predict(const FeatureVector& f) const override {
    return {f.front() > 0 ? SignalAction::kBuy : SignalAction::kHold, 1.0};
  }
};

std::vector<Tick> series(const std::vector<Money>& prices) {
  std::vector<Tick> ticks;
  for (Money p : prices) {
    ticks.push_back(Tick{"700.HK", p, 1, std::chrono::system_clock::now()});
  }
  return ticks;
}

}  // namespace

TEST(Backtester, TooFewTicksYieldsEmptyMetrics) {
  const auto m = Backtester().run(series({100, 101}), AlwaysBuy{});
  EXPECT_EQ(m.totalReturn, 0.0);
  EXPECT_EQ(m.trades, 0U);
}

TEST(Backtester, BuyAndHoldAfterFirstBarMatchesPriceMove) {
  // The position is decided from bar i-1 and applied at bar i, so the first bar is not traded.
  // Prices 100,110,121,133.1: returns 10%,10%,10%; only the last two are earned.
  const auto m = Backtester().run(series({100, 110, 121, 133}), AlwaysBuy{});
  EXPECT_NEAR(m.totalReturn, (121.0 / 110.0) * (133.0 / 121.0) - 1.0, 1e-12);
  EXPECT_EQ(m.trades, 1U);
  EXPECT_DOUBLE_EQ(m.maxDrawdown, 0.0);
}

TEST(Backtester, NoLookaheadPeekerDoesNotProfitFromSameBarReturn) {
  // Alternating up/down series. A peeker acting on the same-bar return would earn every up-bar;
  // with proper lag it trades the *next* bar, which is a down-bar every time.
  const auto m = Backtester().run(series({100, 110, 100, 110, 100, 110, 100}), Peeker{});
  EXPECT_LT(m.totalReturn, 0.0);
}

TEST(Backtester, FeesReduceReturn) {
  const auto ticks = series({100, 101, 102, 103, 104});
  const auto free = Backtester().run(ticks, AlwaysBuy{}, {.periodsPerYear = 252.0, .feeBps = 0.0});
  const auto paid = Backtester().run(ticks, AlwaysBuy{}, {.periodsPerYear = 252.0, .feeBps = 50.0});
  EXPECT_LT(paid.totalReturn, free.totalReturn);
}

TEST(Backtester, SharpeIsNotATautology) {
  // Regression: sharpe was total_return / |total_return| = +/-1 for any non-zero result.
  const auto m = Backtester().run(series({100, 101, 103, 102, 106, 105, 109, 111}), AlwaysBuy{});
  EXPECT_GT(m.annualizedVolatility, 0.0);
  EXPECT_NE(m.sharpe, 1.0);
  EXPECT_NE(m.sharpe, -1.0);
}

TEST(Backtester, DrawdownIsMeasuredFromPeak) {
  // Buy and hold: equity 1.0 -> 1.1 -> 0.99 (the first bar is not traded, so 1.0 -> 1.1 -> 0.99).
  const auto m = Backtester().run(series({100, 100, 110, 99}), AlwaysBuy{});
  EXPECT_NEAR(m.maxDrawdown, 0.1, 1e-12);
}
