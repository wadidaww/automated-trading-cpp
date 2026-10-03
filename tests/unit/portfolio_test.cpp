#include <gtest/gtest.h>

#include <random>

#include "futu_trader/portfolio/fees.hpp"
#include "futu_trader/portfolio/position_book.hpp"

using namespace futu_trader;
using namespace futu_trader::portfolio;

namespace {
FillRecord fill(const std::string& id, Side side, std::int64_t qty, Money price, Money fee = 0,
                const std::string& symbol = "00700") {
  return {id, symbol, side, qty, price, fee};
}
}  // namespace

// --- Fees: golden values computed by hand -----------------------------------------------------

TEST(HkFee, GoldenBuy200At350Point20) {
  // Turnover 200 * 350.20 = 70,040.00 HKD = 70,040,000 mills.
  //   stamp   0.1000%   = 70.04     -> rounded UP to whole HKD  = 71.00
  //   SFC     0.0027%   = 1.891080  -> 1.89
  //   trading 0.00565%  = 3.957260  -> 3.96
  //   FRC     0.00015%  = 0.105060  -> 0.11
  //   settle  0.002%    = 1.4008    -> below the HKD 2 minimum -> 2.00
  //   total = 71.00 + 1.89 + 3.96 + 0.11 + 2.00 = 78.96 HKD
  EXPECT_EQ(hkFee(HkFeeSchedule{}, 70'040'000), 78'960);
}

TEST(HkFee, StampDutyRoundsUpNotToNearest) {
  HkFeeSchedule only;
  only.sfcLevyPpb = only.tradingFeePpb = only.frcLevyPpb = only.settlementPpb = 0;
  only.settlementMinMills = 0;
  // 0.1% of 1,001.00 HKD = 1.001 -> up to 2.00 (not 1.00).
  EXPECT_EQ(hkFee(only, 1'001'000), 2'000);
  // Exactly 1.000 stays 1.00.
  EXPECT_EQ(hkFee(only, 1'000'000), 1'000);
}

TEST(HkFee, SettlementFeeIsClampedBothWays) {
  HkFeeSchedule s;
  s.stampDutyPpb = s.sfcLevyPpb = s.tradingFeePpb = s.frcLevyPpb = 0;
  EXPECT_EQ(hkFee(s, 100'000), 2'000);           // 0.002% of 100 = 0.002 -> min 2.00
  EXPECT_EQ(hkFee(s, 10'000'000'000), 100'000);  // 0.002% of 10,000,000 = 200 -> max 100
}

TEST(HkFee, CommissionMinimumAndPlatformFee) {
  HkFeeSchedule s;
  s.stampDutyPpb = s.sfcLevyPpb = s.tradingFeePpb = s.frcLevyPpb = s.settlementPpb = 0;
  s.settlementMinMills = 0;
  s.commissionPpb = 300'000;  // 0.03%
  s.commissionMinMills = 3'000;
  s.platformFeeMills = 15'000;
  EXPECT_EQ(hkFee(s, 1'000'000), 3'000 + 15'000);     // 0.03% of 1000 = 0.30 -> min 3.00
  EXPECT_EQ(hkFee(s, 100'000'000), 30'000 + 15'000);  // 0.03% of 100,000 = 30.00
}

TEST(HkFee, NonPositiveTurnoverCostsNothing) {
  EXPECT_EQ(hkFee(HkFeeSchedule{}, 0), 0);
  EXPECT_EQ(hkFee(HkFeeSchedule{}, -5), 0);
}

// --- Position book ------------------------------------------------------------------------------

TEST(PositionBook, LongRoundTripRealizesExactPnl) {
  PositionBook book;
  ASSERT_TRUE(book.applyFill(fill("1", Side::kBuy, 100, 350'000)).value());
  EXPECT_EQ(book.qty("00700"), 100);
  EXPECT_EQ(book.snapshot("00700").costBasis, 35'000'000);
  ASSERT_TRUE(book.applyFill(fill("2", Side::kSell, 100, 352'000)).value());
  EXPECT_EQ(book.qty("00700"), 0);
  EXPECT_EQ(book.snapshot("00700").costBasis, 0);
  EXPECT_EQ(book.totalRealizedPnl(), 200'000);  // 100 * 2.00 HKD
}

TEST(PositionBook, WeightedAverageAndPartialClose) {
  PositionBook book;
  ASSERT_TRUE(book.applyFill(fill("1", Side::kBuy, 100, 100'000)).ok());
  ASSERT_TRUE(book.applyFill(fill("2", Side::kBuy, 300, 104'000)).ok());
  // basis = 100*100.000 + 300*104.000 = 41,200,000 ; avg 103.000 over 400 shares
  EXPECT_EQ(book.snapshot("00700").costBasis, 41'200'000);
  ASSERT_TRUE(book.applyFill(fill("3", Side::kSell, 100, 110'000)).ok());
  // released cost = 41,200,000 * 100/400 = 10,300,000 ; proceeds 11,000,000
  EXPECT_EQ(book.totalRealizedPnl(), 700'000);
  EXPECT_EQ(book.snapshot("00700").costBasis, 30'900'000);
  EXPECT_EQ(book.qty("00700"), 300);
}

TEST(PositionBook, ShortAndCover) {
  PositionBook book;
  ASSERT_TRUE(book.applyFill(fill("1", Side::kSell, 200, 50'000)).ok());
  EXPECT_EQ(book.qty("00700"), -200);
  ASSERT_TRUE(book.applyFill(fill("2", Side::kBuy, 200, 48'000)).ok());
  EXPECT_EQ(book.qty("00700"), 0);
  EXPECT_EQ(book.totalRealizedPnl(), 400'000);  // sold high, covered lower: +2.00 * 200
}

TEST(PositionBook, FillCrossingZeroSplitsIntoCloseAndOpen) {
  PositionBook book;
  ASSERT_TRUE(book.applyFill(fill("1", Side::kBuy, 100, 100'000)).ok());
  ASSERT_TRUE(book.applyFill(fill("2", Side::kSell, 250, 102'000)).ok());
  // closes 100 long (+2.00 * 100) and opens a 150 short at 102.000
  EXPECT_EQ(book.qty("00700"), -150);
  EXPECT_EQ(book.totalRealizedPnl(), 200'000);
  EXPECT_EQ(book.snapshot("00700").costBasis, 150 * 102'000);
}

TEST(PositionBook, DuplicateFillIdIsIgnored) {
  PositionBook book;
  ASSERT_TRUE(book.applyFill(fill("dup", Side::kBuy, 100, 100'000, 500)).value());
  const auto again = book.applyFill(fill("dup", Side::kBuy, 100, 100'000, 500));
  ASSERT_TRUE(again.ok());
  EXPECT_FALSE(again.value());
  EXPECT_EQ(book.qty("00700"), 100);
  EXPECT_EQ(book.totalFees(), 500);
}

TEST(PositionBook, CashDeltaIncludesFees) {
  PositionBook book;
  ASSERT_TRUE(book.applyFill(fill("1", Side::kBuy, 100, 100'000, 1'000)).ok());
  EXPECT_EQ(book.cashDelta(), -(100 * 100'000) - 1'000);
  ASSERT_TRUE(book.applyFill(fill("2", Side::kSell, 100, 101'000, 1'000)).ok());
  EXPECT_EQ(book.cashDelta(), -(100 * 100'000) - 1'000 + (100 * 101'000) - 1'000);
}

TEST(PositionBook, UnrealizedAndExposure) {
  PositionBook book;
  ASSERT_TRUE(book.applyFill(fill("1", Side::kBuy, 100, 100'000)).ok());
  EXPECT_EQ(book.unrealizedPnl("00700", 103'000).value(), 300'000);
  EXPECT_EQ(book.exposure("00700", 103'000).value(), 10'300'000);
  ASSERT_TRUE(book.applyFill(fill("2", Side::kSell, 300, 100'000, 0, "09988")).ok());
  EXPECT_EQ(book.unrealizedPnl("09988", 99'000).value(), 300'000);  // short gains when price falls
  EXPECT_EQ(book.exposure("09988", 99'000).value(), -29'700'000);
  const auto gross = book.grossExposure({{"00700", 103'000}, {"09988", 99'000}});
  ASSERT_TRUE(gross.ok());
  EXPECT_EQ(gross.value(), 10'300'000 + 29'700'000);
  EXPECT_FALSE(book.grossExposure({{"00700", 103'000}}).ok());  // missing mark is an error
}

TEST(PositionBook, FillWithoutAnIdIsInvalidBecauseItCouldNotBeDeduplicated) {
  PositionBook book;
  EXPECT_FALSE(book.applyFill(fill("", Side::kBuy, 100, 100'000)).ok());
  EXPECT_TRUE(book.all().empty());
}

TEST(PositionBook, MarkFillSeenSuppressesLaterApplication) {
  PositionBook book;
  book.markFillSeen("already-in-seed");
  const auto result = book.applyFill(fill("already-in-seed", Side::kBuy, 100, 100'000));
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value());
  EXPECT_EQ(book.qty("00700"), 0);
}

TEST(PositionBook, InvalidFillsAreRejectedWithoutChangingState) {
  PositionBook book;
  EXPECT_FALSE(book.applyFill(fill("1", Side::kBuy, 0, 100'000)).ok());
  EXPECT_FALSE(book.applyFill(fill("2", Side::kBuy, 10, 0)).ok());
  EXPECT_FALSE(book.applyFill(fill("3", Side::kBuy, 10, 100'000, -1)).ok());
  EXPECT_FALSE(book.applyFill(fill("4", Side::kBuy, 10, 100'000, 0, "")).ok());
  EXPECT_TRUE(book.all().empty());
}

TEST(PositionBook, OverflowIsRejectedAndLeavesStateUntouched) {
  PositionBook book;
  ASSERT_TRUE(book.applyFill(fill("1", Side::kBuy, 10, 100'000)).ok());
  const auto huge = book.applyFill(fill("2", Side::kBuy, INT64_MAX / 2, INT64_MAX / 2));
  EXPECT_FALSE(huge.ok());
  EXPECT_EQ(book.qty("00700"), 10);
  EXPECT_EQ(book.snapshot("00700").costBasis, 1'000'000);
  // The failed fill's id must not be remembered, or a corrected resend would be dropped.
  EXPECT_TRUE(book.applyFill(fill("2", Side::kBuy, 1, 100'000)).value());
}

TEST(PositionBook, RandomFillsKeepInvariants) {
  // Property: (a) flat positions carry zero cost basis, (b) cash + realized/unrealized relations
  // hold: cashDelta == realized - open basis (long) ... checked via total value identity.
  std::mt19937 rng(5);
  for (int round = 0; round < 300; ++round) {
    PositionBook book;
    for (int i = 0; i < 40; ++i) {
      const Side side = rng() % 2 ? Side::kBuy : Side::kSell;
      const std::int64_t qty = 1 + static_cast<std::int64_t>(rng() % 300);
      const Money price = 90'000 + static_cast<Money>(rng() % 20'000);
      ASSERT_TRUE(
          book.applyFill(fill(std::to_string(round) + "-" + std::to_string(i), side, qty, price))
              .ok());
      const auto snap = book.snapshot("00700");
      if (snap.qty == 0) {
        ASSERT_EQ(snap.costBasis, 0);  // no residue when flat
      }
      ASSERT_GE(snap.costBasis, 0);
    }
    // With zero fees: cash = realized pnl - (cost of open position, signed by direction).
    const auto snap = book.snapshot("00700");
    const Money openCost = snap.qty >= 0 ? snap.costBasis : -snap.costBasis;
    ASSERT_EQ(book.cashDelta(), snap.realizedPnl - openCost);
  }
}

TEST(PositionBook, SeedPositionCarriesOverWithoutPnlOrCash) {
  PositionBook book;
  ASSERT_TRUE(book.seedPosition("00700", 200, 340'000).value());
  EXPECT_EQ(book.qty("00700"), 200);
  EXPECT_EQ(book.snapshot("00700").costBasis, 68'000'000);
  EXPECT_EQ(book.cashDelta(), 0);
  EXPECT_EQ(book.totalRealizedPnl(), 0);
  // Selling part of it realizes P&L against the seeded cost.
  ASSERT_TRUE(book.applyFill(fill("1", Side::kSell, 100, 350'000)).ok());
  EXPECT_EQ(book.totalRealizedPnl(), 1'000'000);
  // Cannot seed over an open position, and bad inputs are rejected.
  EXPECT_FALSE(book.seedPosition("00700", 50, 340'000).ok());
  EXPECT_FALSE(book.seedPosition("09988", 0, 340'000).ok());
  EXPECT_FALSE(book.seedPosition("09988", 10, 0).ok());
  ASSERT_TRUE(book.seedPosition("09988", -100, 80'000).value());  // seeded short
  EXPECT_EQ(book.qty("09988"), -100);
}
