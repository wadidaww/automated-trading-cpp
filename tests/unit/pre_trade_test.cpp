#include "futu_trader/oms/pre_trade.hpp"

#include <gtest/gtest.h>

using namespace futu_trader;
using namespace futu_trader::oms;

namespace {

struct Fixture {
  Fixture() {
    instruments.add({"00700", 100});
    instruments.add({"00005", 400});
  }
  PreTradeRisk make(PreTradeConfig cfg = defaultCfg()) {
    return PreTradeRisk({.maxPositionNotionalMinor = 200'000'000,
                         .maxPortfolioNotionalMinor = 1'000'000'000,
                         .maxDailyLossMinor = 5'000'000,
                         .maxOpenOrders = 10,
                         .concentrationLimit = 1.0},
                        cfg, killSwitch, instruments);
  }
  static PreTradeConfig defaultCfg() {
    return {.priceBandBps = 200,
            .maxQuoteAgeMs = 2000,
            .maxOrderNotionalMills = 100'000'000,
            .allowShort = false};
  }
  static Order order(Side side = Side::kBuy, std::int64_t qty = 100, Money price = 350'000) {
    Order o;
    o.symbol = "00700";
    o.side = side;
    o.quantity = qty;
    o.limitPriceMinor = price;
    return o;
  }
  static QuoteContext quote(Money last = 350'000, std::int64_t ageMs = 100) {
    return {last, 1'000'000'000, 1'000'000'000 + (ageMs * 1'000'000)};
  }
  static AccountRiskState state(std::int64_t held = 0) {
    AccountRiskState s;
    s.heldQty = held;
    return s;
  }
  execution::KillSwitch killSwitch;
  instrument::InstrumentTable instruments;
};

}  // namespace

TEST(PreTrade, AcceptsAWellFormedOrder) {
  Fixture f;
  EXPECT_EQ(f.make().check(Fixture::order(), Fixture::quote(), Fixture::state()), RiskReject::kOk);
}

TEST(PreTrade, KillSwitchBlocksEverythingFirst) {
  Fixture f;
  f.killSwitch.trip("test");
  EXPECT_EQ(f.make().check(Fixture::order(), Fixture::quote(), Fixture::state()),
            RiskReject::kKillSwitch);
  f.killSwitch.reset("op");
  EXPECT_EQ(f.make().check(Fixture::order(), Fixture::quote(), Fixture::state()), RiskReject::kOk);
}

TEST(PreTrade, UnknownInstrumentAndLotSize) {
  Fixture f;
  auto unknown = Fixture::order();
  unknown.symbol = "99999";
  EXPECT_EQ(f.make().check(unknown, Fixture::quote(), Fixture::state()),
            RiskReject::kUnknownInstrument);
  EXPECT_EQ(f.make().check(Fixture::order(Side::kBuy, 150), Fixture::quote(), Fixture::state()),
            RiskReject::kLotSize);
  auto hsbc = Fixture::order(Side::kBuy, 200, 350'000);  // 00005 lot is 400
  hsbc.symbol = "00005";
  EXPECT_EQ(f.make().check(hsbc, Fixture::quote(), Fixture::state()), RiskReject::kLotSize);
}

TEST(PreTrade, TickAlignment) {
  Fixture f;
  // 350.10 is not a multiple of the 0.20 tick in the 200-500 band.
  EXPECT_EQ(f.make().check(Fixture::order(Side::kBuy, 100, 350'100), Fixture::quote(350'100),
                           Fixture::state()),
            RiskReject::kTickSize);
}

TEST(PreTrade, PriceBandBoundaryIsInclusive) {
  Fixture f;
  // 2% band around 350.000: 357.000 is exactly on the edge (aligned to the 0.20 tick).
  EXPECT_EQ(
      f.make().check(Fixture::order(Side::kBuy, 100, 357'000), Fixture::quote(), Fixture::state()),
      RiskReject::kOk);
  EXPECT_EQ(
      f.make().check(Fixture::order(Side::kBuy, 100, 357'200), Fixture::quote(), Fixture::state()),
      RiskReject::kPriceBand);
  EXPECT_EQ(f.make().check(Fixture::order(Side::kSell, 100, 342'800), Fixture::quote(),
                           Fixture::state(100)),
            RiskReject::kPriceBand);
}

TEST(PreTrade, StaleOrMissingQuoteFailsClosed) {
  Fixture f;
  EXPECT_EQ(f.make().check(Fixture::order(), Fixture::quote(350'000, 2000), Fixture::state()),
            RiskReject::kOk);  // exactly at the limit
  EXPECT_EQ(f.make().check(Fixture::order(), Fixture::quote(350'000, 2001), Fixture::state()),
            RiskReject::kStaleQuote);
  EXPECT_EQ(f.make().check(Fixture::order(), Fixture::quote(0), Fixture::state()),
            RiskReject::kStaleQuote);
  // A quote "from the future" (clock confusion) is not trusted either.
  QuoteContext future{350'000, 5'000'000'000, 1'000'000'000};
  EXPECT_EQ(f.make().check(Fixture::order(), future, Fixture::state()), RiskReject::kStaleQuote);
}

TEST(PreTrade, FatFingerNotionalCap) {
  Fixture f;
  // 100'000 HKD cap = 100,000,000 mills; 300 shares @ 350 = 105,000,000.
  auto big = Fixture::order(Side::kBuy, 300, 350'000);
  EXPECT_EQ(f.make().check(big, Fixture::quote(), Fixture::state()), RiskReject::kMaxOrderNotional);
  PreTradeConfig unset = Fixture::defaultCfg();
  unset.maxOrderNotionalMills = 0;  // unset cap must reject, not mean "unlimited"
  EXPECT_EQ(f.make(unset).check(Fixture::order(), Fixture::quote(), Fixture::state()),
            RiskReject::kMaxOrderNotional);
}

TEST(PreTrade, SellingMoreThanHeldIsAShortAndBlockedByDefault) {
  Fixture f;
  EXPECT_EQ(f.make().check(Fixture::order(Side::kSell, 100), Fixture::quote(), Fixture::state(0)),
            RiskReject::kShortSale);
  EXPECT_EQ(f.make().check(Fixture::order(Side::kSell, 200), Fixture::quote(), Fixture::state(100)),
            RiskReject::kShortSale);
  EXPECT_EQ(f.make().check(Fixture::order(Side::kSell, 100), Fixture::quote(), Fixture::state(100)),
            RiskReject::kOk);  // closing a long is fine
  PreTradeConfig shorting = Fixture::defaultCfg();
  shorting.allowShort = true;
  EXPECT_EQ(
      f.make(shorting).check(Fixture::order(Side::kSell, 100), Fixture::quote(), Fixture::state(0)),
      RiskReject::kOk);
}

TEST(PreTrade, FallsThroughToPositionAndAccountLimits) {
  Fixture f;
  auto s = Fixture::state();
  s.liveOrders = 10;
  EXPECT_EQ(f.make().check(Fixture::order(), Fixture::quote(), s), RiskReject::kMaxOpenOrders);
  s = Fixture::state();
  s.dailyPnl = -5'000'001;
  EXPECT_EQ(f.make().check(Fixture::order(), Fixture::quote(), s), RiskReject::kDailyLoss);
  s = Fixture::state();
  s.exposure["00700"] = 190'000'000;  // 35,000,000 more breaches the 200,000,000 per-symbol cap
  s.grossNotional = 190'000'000;
  EXPECT_EQ(f.make().check(Fixture::order(), Fixture::quote(), s), RiskReject::kPositionLimit);
}

TEST(PreTrade, EveryRejectReasonHasAName) {
  for (auto reason : {RiskReject::kKillSwitch, RiskReject::kUnknownInstrument, RiskReject::kLotSize,
                      RiskReject::kTickSize, RiskReject::kPriceBand, RiskReject::kStaleQuote,
                      RiskReject::kMaxOrderNotional, RiskReject::kShortSale, RiskReject::kRateLimit,
                      RiskReject::kUnresolvedOrder}) {
    EXPECT_STRNE(toString(reason), "unknown");
  }
}
