#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>

#include "futu_trader/risk/risk_engine.hpp"

using namespace futu_trader;

namespace {

using Positions = std::unordered_map<std::string, Money>;

RiskConfig limits() {
  return {.maxPositionNotionalMinor = 50000,
          .maxPortfolioNotionalMinor = 100000,
          .maxDailyLossMinor = 10000,
          .maxOpenOrders = 10,
          .concentrationLimit = 0.8};
}

Order makeOrder(Side side, std::int64_t qty, Money price) {
  Order o;
  o.symbol = "700.HK";
  o.side = side;
  o.quantity = qty;
  o.limitPriceMinor = price;
  return o;
}

}  // namespace

TEST(RiskEngine, AcceptsOrderWithinLimits) {
  RiskEngine risk(limits());
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 1, 10000), {}, 0, 0, 0), RiskReject::kOk);
}

TEST(RiskEngine, DefaultConfigFailsClosed) {
  RiskEngine risk(RiskConfig{});
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 1, 10000), {}, 0, 0, 0), RiskReject::kMaxOpenOrders);
}

TEST(RiskEngine, RejectsInvalidOrders) {
  RiskEngine risk(limits());
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 0, 10000), {}, 0, 0, 0), RiskReject::kInvalidOrder);
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, -5, 10000), {}, 0, 0, 0), RiskReject::kInvalidOrder);
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 1, 0), {}, 0, 0, 0), RiskReject::kInvalidOrder);
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 1, -1), {}, 0, 0, 0), RiskReject::kInvalidOrder);
}

TEST(RiskEngine, PositionLimitBoundary) {
  RiskEngine risk(limits());
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 5, 10000), {}, 0, 0, 0), RiskReject::kOk);  // == cap
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 6, 10000), {}, 0, 0, 0), RiskReject::kPositionLimit);
}

TEST(RiskEngine, ExistingPositionCountsTowardsLimit) {
  RiskEngine risk(limits());
  const Positions pos{{"700.HK", 40000}};
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 2, 10000), pos, 40000, 0, 0),
            RiskReject::kPositionLimit);
}

TEST(RiskEngine, SellAgainstLongReducesExposureAndIsAllowed) {
  // Regression: a sell used to be counted as added notional and could be rejected.
  RiskEngine risk(limits());
  const Positions pos{{"700.HK", 50000}};
  EXPECT_EQ(risk.check(makeOrder(Side::kSell, 3, 10000), pos, 100000, 0, 0), RiskReject::kOk);
}

TEST(RiskEngine, ShortExposureIsLimitedByMagnitude) {
  RiskEngine risk(limits());
  const Positions pos{{"700.HK", -40000}};
  EXPECT_EQ(risk.check(makeOrder(Side::kSell, 2, 10000), pos, 40000, 0, 0),
            RiskReject::kPositionLimit);
}

TEST(RiskEngine, FlippingThroughZeroIsCheckedOnTheNewSide) {
  RiskEngine risk(limits());
  const Positions pos{{"700.HK", 10000}};
  // sell 7 lots @10000: from +10000 to -60000, magnitude exceeds the cap.
  EXPECT_EQ(risk.check(makeOrder(Side::kSell, 7, 10000), pos, 10000, 0, 0),
            RiskReject::kPositionLimit);
}

TEST(RiskEngine, PortfolioLimit) {
  RiskConfig cfg = limits();
  cfg.maxPortfolioNotionalMinor = 60000;
  RiskEngine risk(cfg);
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 2, 10000), {}, 50000, 0, 0),
            RiskReject::kPortfolioLimit);
}

TEST(RiskEngine, ConcentrationLimit) {
  RiskConfig cfg = limits();
  cfg.concentrationLimit = 0.3;  // 30% of 100000 = 30000
  RiskEngine risk(cfg);
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 4, 10000), {}, 0, 0, 0), RiskReject::kConcentration);
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 3, 10000), {}, 0, 0, 0), RiskReject::kOk);
}

TEST(RiskEngine, OpenOrderLimit) {
  RiskEngine risk(limits());
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 1, 10000), {}, 0, 0, 9), RiskReject::kOk);
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 1, 10000), {}, 0, 0, 10), RiskReject::kMaxOpenOrders);
}

TEST(RiskEngine, DailyLossBoundary) {
  RiskEngine risk(limits());
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 1, 10000), {}, 0, -10000, 0), RiskReject::kOk);
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 1, 10000), {}, 0, -10001, 0), RiskReject::kDailyLoss);
  EXPECT_EQ(
      risk.check(makeOrder(Side::kBuy, 1, 10000), {}, 0, std::numeric_limits<Money>::min(), 0),
      RiskReject::kDailyLoss);
}

TEST(RiskEngine, OverflowIsRejectedNotWrapped) {
  RiskEngine risk(limits());
  const Money huge = std::numeric_limits<Money>::max() / 2 + 1;
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 4, huge), {}, 0, 0, 0), RiskReject::kOverflow);
}

TEST(RiskEngine, ReasonStrings) {
  EXPECT_STREQ(toString(RiskReject::kOk), "ok");
  EXPECT_STREQ(toString(RiskReject::kDailyLoss), "daily_loss");
}

TEST(Kelly, KnownValues) {
  EXPECT_DOUBLE_EQ(KellyCriterion::fraction(0.5, 0.0), 0.0);
  EXPECT_NEAR(KellyCriterion::fraction(0.6, 1.0), 0.2, 1e-12);
  EXPECT_DOUBLE_EQ(KellyCriterion::fraction(0.3, 1.0), 0.0);  // negative edge clamps to zero
}

TEST(DrawdownMonitor, TracksPeakToTrough) {
  DrawdownMonitor dd;
  dd.observe(100);
  dd.observe(120);
  dd.observe(90);
  dd.observe(110);
  EXPECT_NEAR(dd.maxDrawdown(), 0.25, 1e-12);
}

TEST(RiskEngine, FlatteningIsAllowedEvenWhenTheDailyLossAndOrderLimitsAreBreached) {
  // After a breach the system must still be able to reduce risk.
  RiskEngine risk(limits());
  const Positions pos{{"700.HK", 50000}};
  const Order sell = makeOrder(Side::kSell, 3, 10000);
  EXPECT_EQ(risk.check(sell, pos, 50000, -9'999'999, 10), RiskReject::kOk);  // loss + full orders
  // The same conditions block an order that ADDS exposure.
  const Order buy = makeOrder(Side::kBuy, 1, 10000);
  EXPECT_EQ(risk.check(buy, pos, 50000, -9'999'999, 0), RiskReject::kDailyLoss);
  EXPECT_EQ(risk.check(buy, pos, 50000, 0, 10), RiskReject::kMaxOpenOrders);
}

TEST(RiskEngine, ReducingOrdersStillGetOverflowAndValidityChecks) {
  RiskEngine risk(limits());
  const Positions pos{{"700.HK", 50000}};
  EXPECT_EQ(risk.check(makeOrder(Side::kSell, 0, 10000), pos, 50000, 0, 0),
            RiskReject::kInvalidOrder);
  const Money huge = std::numeric_limits<Money>::max() / 2 + 1;
  EXPECT_EQ(risk.check(makeOrder(Side::kSell, 4, huge), pos, 50000, 0, 0), RiskReject::kOverflow);
}

TEST(RiskEngine, UnsetConcentrationLimitFailsClosed) {
  RiskConfig cfg = limits();
  cfg.concentrationLimit = 0.0;  // unset must not mean "unlimited"
  RiskEngine risk(cfg);
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 1, 10000), {}, 0, 0, 0), RiskReject::kConcentration);
  EXPECT_EQ(RiskConfig{}.concentrationLimit, 0.0);
}

TEST(RiskEngine, UnsetDailyLossLimitFailsClosed) {
  RiskConfig cfg = limits();
  cfg.maxDailyLossMinor = 0;
  RiskEngine risk(cfg);
  EXPECT_EQ(risk.check(makeOrder(Side::kBuy, 1, 10000), {}, 0, 0, 0), RiskReject::kDailyLoss);
}
