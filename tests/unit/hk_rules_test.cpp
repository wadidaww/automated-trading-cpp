#include "futu_trader/instrument/hk_rules.hpp"

#include <gtest/gtest.h>

using namespace futu_trader;
using namespace futu_trader::instrument;

TEST(HkTick, BandValuesAndBoundaries) {
  // (price in HKD -> tick in HKD), all expressed in mills.
  EXPECT_EQ(hkEquityTick(10), 1);   // 0.010
  EXPECT_EQ(hkEquityTick(249), 1);  // 0.249
  EXPECT_EQ(hkEquityTick(250), 5);  // 0.250 starts the 0.005 band
  EXPECT_EQ(hkEquityTick(499), 5);
  EXPECT_EQ(hkEquityTick(500), 10);  // 0.50
  EXPECT_EQ(hkEquityTick(9'990), 10);
  EXPECT_EQ(hkEquityTick(10'000), 20);       // 10.00
  EXPECT_EQ(hkEquityTick(20'000), 50);       // 20.00
  EXPECT_EQ(hkEquityTick(100'000), 100);     // 100.00
  EXPECT_EQ(hkEquityTick(200'000), 200);     // 200.00
  EXPECT_EQ(hkEquityTick(350'000), 200);     // Tencent-like 350.00
  EXPECT_EQ(hkEquityTick(500'000), 500);     // 500.00
  EXPECT_EQ(hkEquityTick(1'000'000), 1000);  // 1000.00
  EXPECT_EQ(hkEquityTick(2'000'000), 2000);
  EXPECT_EQ(hkEquityTick(5'000'000), 5000);
  EXPECT_EQ(hkEquityTick(9'995'000), 5000);  // last valid price
}

TEST(HkTick, OutOfRangeHasNoTick) {
  EXPECT_EQ(hkEquityTick(0), 0);
  EXPECT_EQ(hkEquityTick(9), 0);
  EXPECT_EQ(hkEquityTick(-100), 0);
  EXPECT_EQ(hkEquityTick(9'995'001), 0);
}

TEST(HkTick, AlignmentAcrossBands) {
  EXPECT_TRUE(isTickAligned(350'200));   // 350.20 with 0.20 tick
  EXPECT_FALSE(isTickAligned(350'100));  // 350.10 is not a multiple of 0.20
  EXPECT_TRUE(isTickAligned(25'050));    // 25.05, tick 0.05
  EXPECT_FALSE(isTickAligned(25'020));   // 25.02
  EXPECT_TRUE(isTickAligned(400));       // 0.400, tick 0.005
  EXPECT_FALSE(isTickAligned(401));
  EXPECT_FALSE(isTickAligned(5));  // below range
  EXPECT_FALSE(isTickAligned(0));
}

TEST(HkTick, EveryBandBoundaryIsAlignedUnderBothAdjacentTicks) {
  // A boundary price must be valid whichever side's tick you use, otherwise the table is wrong.
  for (Money boundary :
       {250, 500, 10'000, 20'000, 100'000, 200'000, 500'000, 1'000'000, 2'000'000, 5'000'000}) {
    EXPECT_TRUE(isTickAligned(boundary)) << boundary;
    EXPECT_TRUE(isTickAligned(boundary - hkEquityTick(boundary - 1))) << boundary;
  }
}

TEST(InstrumentTable, LotSizesComeFromData) {
  InstrumentTable table;
  table.add({"00700", 100});
  ASSERT_TRUE(table.find("00700").has_value());
  EXPECT_EQ(table.find("00700")->lotSize, 100);
  EXPECT_FALSE(table.find("09999").has_value());
}
