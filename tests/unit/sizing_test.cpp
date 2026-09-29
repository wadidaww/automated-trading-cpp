#include "futu_trader/sizing/sizing.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace futu_trader;
using namespace futu_trader::sizing;

namespace {
// Equity 1,000,000 HKD (in mills), price 350 HKD, lot 100 => one lot is 35,000 HKD.
constexpr Money kEquity = 1'000'000'000;
constexpr Money kPrice = 350'000;
SizingLimits limits() {
  return {.lotSize = 100, .maxOrderNotionalMills = 500'000'000, .maxEquityFraction = 0.5};
}
}  // namespace

TEST(Kelly, SizesFromFractionalKellyAndRoundsDownToLots) {
  // p=0.6, b=1 -> full Kelly 0.2; quarter Kelly 0.05 -> 50,000 HKD -> 1 lot of 35,000 HKD.
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.6, 1.0, 0.25, limits()), 100);
  // half Kelly 0.10 -> 100,000 HKD -> 2 lots (70,000), the remainder is dropped.
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.6, 1.0, 0.5, limits()), 200);
}

TEST(Kelly, NoEdgeOrBadInputsMeanNoTrade) {
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.4, 1.0, 0.25, limits()), 0);  // negative edge
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.0, 1.0, 0.25, limits()), 0);
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 1.0, 1.0, 0.25, limits()), 0);  // certainty is a bug
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, std::nan(""), 1.0, 0.25, limits()), 0);
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.6, 0.0, 0.25, limits()), 0);
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.6, 1.0, 0.0, limits()), 0);
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.6, 1.0, 1.5, limits()), 0);  // >1x Kelly refused
  EXPECT_EQ(kellyQuantity(0, kPrice, 0.6, 1.0, 0.25, limits()), 0);
  EXPECT_EQ(kellyQuantity(kEquity, 0, 0.6, 1.0, 0.25, limits()), 0);
}

TEST(Kelly, CapsAlwaysWin) {
  SizingLimits tight = limits();
  tight.maxEquityFraction = 0.04;  // 40,000 HKD cap -> 1 lot even though Kelly asks for 2
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.6, 1.0, 0.5, tight), 100);
  tight.maxOrderNotionalMills = 30'000'000;  // 30,000 HKD < one lot -> nothing fits
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.6, 1.0, 0.5, tight), 0);
}

TEST(Kelly, UnsetLimitsMeanNoTradeNotUnlimited) {
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.6, 1.0, 0.25, SizingLimits{}), 0);
  SizingLimits noLot = limits();
  noLot.lotSize = 0;
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.6, 1.0, 0.25, noLot), 0);
  SizingLimits noCap = limits();
  noCap.maxOrderNotionalMills = 0;
  EXPECT_EQ(kellyQuantity(kEquity, kPrice, 0.6, 1.0, 0.25, noCap), 0);
}

TEST(VolTarget, SizesDownWhenTheInstrumentIsMoreVolatile) {
  // target 1% daily, instrument 2% -> half of equity would be 500,000 HKD; the 0.5 equity cap and
  // 500,000 order cap allow it: 500,000 / 35,000 = 14.28 lots -> 14 lots = 1400 shares.
  EXPECT_EQ(volTargetQuantity(kEquity, kPrice, 0.01, 0.02, limits()), 1400);
  // Twice the volatility -> half the size: 250,000 / 35,000 = 7.14 -> 7 lots.
  EXPECT_EQ(volTargetQuantity(kEquity, kPrice, 0.01, 0.04, limits()), 700);
}

TEST(VolTarget, LowVolatilityCannotLeverageBeyondTheCaps) {
  // 1% target vs 0.1% instrument would mean 10x equity; the caps bound it at 500,000 HKD.
  EXPECT_EQ(volTargetQuantity(kEquity, kPrice, 0.01, 0.001, limits()), 1400);
}

TEST(VolTarget, BadInputsMeanNoTrade) {
  EXPECT_EQ(volTargetQuantity(kEquity, kPrice, 0.0, 0.02, limits()), 0);
  EXPECT_EQ(volTargetQuantity(kEquity, kPrice, 0.01, 0.0, limits()), 0);
  EXPECT_EQ(volTargetQuantity(kEquity, kPrice, 0.01, -0.02, limits()), 0);
  EXPECT_EQ(
      volTargetQuantity(kEquity, kPrice, 0.01, std::numeric_limits<double>::infinity(), limits()),
      0);
  EXPECT_EQ(volTargetQuantity(-5, kPrice, 0.01, 0.02, limits()), 0);
}

TEST(Sizing, ResultsAreAlwaysWholeLots) {
  for (double vol : {0.007, 0.013, 0.021, 0.034, 0.05}) {
    const auto qty = volTargetQuantity(kEquity, kPrice, 0.01, vol, limits());
    EXPECT_EQ(qty % 100, 0) << vol;
    EXPECT_GE(qty, 0);
  }
}
