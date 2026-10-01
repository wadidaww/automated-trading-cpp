#include "futu_trader/backtest/sim_venue.hpp"

#include <gtest/gtest.h>

using namespace futu_trader;
using namespace futu_trader::backtest;

namespace {

constexpr std::int64_t kMs = 1'000'000;

QuoteEvent quote(std::int64_t tsMs, Money bid, Money ask, std::int64_t bidSize = 0,
                 std::int64_t askSize = 0, const std::string& sym = "00700") {
  return {tsMs * kMs, sym, bid, ask, bid, bidSize, askSize};
}

opend::PlaceOrderRequest order(Side side, std::int64_t qty, Money price,
                               const std::string& remark = "r") {
  opend::PlaceOrderRequest req;
  req.code = "00700";
  req.side = side;
  req.qty = qty;
  req.priceMills = price;
  req.remark = remark;
  return req;
}

struct Fixture {
  explicit Fixture(SimVenueConfig cfg = {}) : venue(std::move(cfg), instruments, clock) {
    instruments.add({"00700", 100});
    venue.setSinks([this](const opend::BrokerOrder& o) { orderEvents.push_back(o); },
                   [this](const opend::BrokerFill& f) { fillEvents.push_back(f); });
  }
  // Moves time to `ms`, runs due actions strictly before the quote, then applies the quote.
  void tick(std::int64_t ms, Money bid, Money ask, std::int64_t bidSize = 0,
            std::int64_t askSize = 0) {
    venue.advance((ms * kMs) - 1);
    clock.setNs(ms * kMs);
    venue.onQuote(quote(ms, bid, ask, bidSize, askSize));
    venue.advance(ms * kMs);
  }
  void at(std::int64_t ms) {
    venue.advance(ms * kMs);
    if (clock.nowNs() < ms * kMs) {
      clock.setNs(ms * kMs);
    }
  }
  opend::BrokerOrder stored(std::uint64_t id) {
    const auto listing = venue.listOrders();  // keep the Result alive while we read from it
    for (const auto& o : listing.value()) {
      if (o.orderId == id) {
        return o;
      }
    }
    return {};
  }

  ManualClock clock;
  instrument::InstrumentTable instruments;
  SimVenue venue;
  std::vector<opend::BrokerOrder> orderEvents;
  std::vector<opend::BrokerFill> fillEvents;
};

}  // namespace

// --- Validation, like a real broker -------------------------------------------------------------

TEST(SimVenue, RefusesWhatARealBrokerWouldRefuse) {
  Fixture f;
  f.tick(0, 350'000, 350'200);
  EXPECT_FALSE(f.venue.place(order(Side::kBuy, 150, 350'200)).ok());  // not a board lot
  EXPECT_FALSE(f.venue.place(order(Side::kBuy, 100, 350'100)).ok());  // off the tick grid
  EXPECT_FALSE(f.venue.place(order(Side::kBuy, 0, 350'200)).ok());
  EXPECT_FALSE(f.venue.place(order(Side::kBuy, 100, 0)).ok());
  auto unknown = order(Side::kBuy, 100, 350'200);
  unknown.code = "99999";
  EXPECT_FALSE(f.venue.place(unknown).ok());
  auto shorting = order(Side::kSell, 100, 350'200);
  shorting.sellShort = true;
  EXPECT_FALSE(f.venue.place(shorting).ok());                          // shorting disabled
  EXPECT_FALSE(f.venue.place(order(Side::kSell, 100, 350'200)).ok());  // nothing to sell
  EXPECT_FALSE(f.venue.place(order(Side::kBuy, 3000, 350'200)).ok());  // 1.05M > 1M cash
  const auto refused = f.venue.place(order(Side::kBuy, 150, 350'200));
  EXPECT_EQ(refused.error().code, ErrorCode::kServer);  // a definite refusal, not ambiguity
  EXPECT_EQ(f.venue.orderCount(), 0U);
}

// --- Matching -----------------------------------------------------------------------------------

TEST(SimVenue, MarketableOrderFillsAtTheTouchOnlyAfterLatency) {
  Fixture f;
  f.tick(0, 350'000, 350'200);
  const auto placed = f.venue.place(order(Side::kBuy, 200, 350'400));  // willing to pay more
  ASSERT_TRUE(placed.ok());
  f.at(49);
  EXPECT_TRUE(f.fillEvents.empty());  // 49 ms: still in flight
  f.at(50);
  ASSERT_EQ(f.fillEvents.size(), 1U);
  EXPECT_EQ(f.fillEvents[0].priceMills, 350'200);  // price improvement: the ask, not the limit
  EXPECT_EQ(f.fillEvents[0].qty, 200);
  EXPECT_EQ(f.stored(placed.value().orderId).status, 11);
  // 200 * 350.200 = 70,040,000 mills turnover; fee 78,960 (hand-computed in the fee tests).
  EXPECT_EQ(f.venue.cash(), 1'000'000'000 - 70'040'000 - 78'960);
  EXPECT_EQ(f.venue.position("00700"), 200);
  EXPECT_EQ(f.venue.totalFees(), 78'960);
}

TEST(SimVenue, LatencyLetsTheMarketMoveAwayBeforeTheOrderArrives) {
  Fixture f;
  f.tick(0, 350'000, 350'200);
  const auto placed = f.venue.place(order(Side::kBuy, 100, 350'200));  // limit == ask when decided
  ASSERT_TRUE(placed.ok());
  f.tick(20, 350'400, 350'600);  // the market ticks up while the order is in flight
  f.at(50);
  EXPECT_TRUE(f.fillEvents.empty());  // it arrives to an ask of 350.6 > its 350.2 limit
  EXPECT_EQ(f.stored(placed.value().orderId).status, 5);  // resting, unfilled
  EXPECT_EQ(f.venue.position("00700"), 0);
}

TEST(SimVenue, PartialFillsAreLimitedByDisplayedSizeAndTheTakerRemainderKeepsTheTouch) {
  Fixture f;
  f.tick(0, 350'000, 350'200, 0, 50);  // only 50 shares displayed at the ask
  const auto placed = f.venue.place(order(Side::kBuy, 200, 350'400));
  ASSERT_TRUE(placed.ok());
  f.at(50);
  ASSERT_EQ(f.fillEvents.size(), 1U);
  EXPECT_EQ(f.fillEvents[0].qty, 50);
  EXPECT_EQ(f.stored(placed.value().orderId).status, 10);  // partially filled
  f.tick(100, 350'000, 350'200, 0, 100);
  ASSERT_EQ(f.fillEvents.size(), 2U);
  EXPECT_EQ(f.fillEvents[1].qty, 100);
  EXPECT_EQ(f.fillEvents[1].priceMills, 350'200);  // the touch, not its 350.4 limit
  f.tick(200, 350'000, 350'200, 0, 120);           // the size changed again: liquidity to work with
  ASSERT_EQ(f.fillEvents.size(), 3U);
  EXPECT_EQ(f.fillEvents[2].qty, 50);  // only 50 left
  EXPECT_EQ(f.stored(placed.value().orderId).status, 11);
  EXPECT_EQ(f.stored(placed.value().orderId).fillQty, 200);
}

TEST(SimVenue, RestingBuyNeedsTheMarketToTradeStrictlyThroughAndFillsAtItsOwnPrice) {
  Fixture f;
  f.tick(0, 350'000, 350'400);
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 100, 350'200)).ok());  // below the ask: passive
  f.at(50);
  EXPECT_TRUE(f.fillEvents.empty());
  f.tick(100, 350'000, 350'200);  // the ask only TOUCHES our price: no evidence of a fill
  EXPECT_TRUE(f.fillEvents.empty());
  f.tick(200, 349'800, 350'000);  // the ask trades through our price
  ASSERT_EQ(f.fillEvents.size(), 1U);
  EXPECT_EQ(f.fillEvents[0].priceMills, 350'200);  // our limit, not the better market price
}

TEST(SimVenue, TouchFillsAreAllowedWhenTheConservativeRuleIsSwitchedOff) {
  SimVenueConfig cfg;
  cfg.passiveNeedsTradeThrough = false;
  Fixture f(cfg);
  f.tick(0, 350'000, 350'400);
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 100, 350'200)).ok());
  f.at(50);
  f.tick(100, 350'000, 350'200);
  EXPECT_EQ(f.fillEvents.size(), 1U);
}

TEST(SimVenue, RestingSellMirrorsTheBuySide) {
  Fixture f;
  f.tick(0, 350'000, 350'200);
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 100, 350'200)).ok());
  f.at(50);
  ASSERT_EQ(f.venue.position("00700"), 100);
  ASSERT_TRUE(f.venue.place(order(Side::kSell, 100, 350'600)).ok());  // above the bid: passive
  f.at(100);
  f.tick(200, 350'600, 350'800);  // bid touches our price: no fill
  EXPECT_EQ(f.fillEvents.size(), 1U);
  f.tick(300, 350'800, 351'000);  // bid trades through
  ASSERT_EQ(f.fillEvents.size(), 2U);
  EXPECT_EQ(f.fillEvents[1].priceMills, 350'600);
  EXPECT_EQ(f.fillEvents[1].side, Side::kSell);
  EXPECT_EQ(f.venue.position("00700"), 0);
}

// --- Liquidity is finite ------------------------------------------------------------------------

TEST(SimVenue, TwoOrdersCannotBothConsumeTheSameDisplayedSize) {
  Fixture f;
  f.tick(0, 350'000, 350'200, 0, 100);  // 100 shares at the ask
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 100, 350'400, "a")).ok());
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 100, 350'400, "b")).ok());
  f.at(50);                            // both arrive together
  ASSERT_EQ(f.fillEvents.size(), 1U);  // the first takes all 100; nothing is left for the second
  EXPECT_EQ(f.fillEvents[0].qty, 100);
  EXPECT_EQ(f.venue.position("00700"), 100);
}

TEST(SimVenue, ARepeatedIdenticalQuoteIsNotNewLiquidity) {
  Fixture f;
  f.tick(0, 350'000, 350'200, 0, 100);
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 200, 350'400)).ok());
  f.at(50);
  ASSERT_EQ(f.fillEvents.size(), 1U);
  f.tick(100, 350'000, 350'200, 0, 100);  // same price, same size: our 100 are still gone
  EXPECT_EQ(f.fillEvents.size(), 1U);
  f.tick(200, 350'000, 350'200, 0, 150);  // the displayed size changed: fresh liquidity
  ASSERT_EQ(f.fillEvents.size(), 2U);
  EXPECT_EQ(f.fillEvents[1].qty, 100);  // only the remaining 100
}

TEST(SimVenue, AMovedQuoteResetsTheConsumedSize) {
  Fixture f;
  f.tick(0, 350'000, 350'200, 0, 100);
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 300, 350'600)).ok());
  f.at(50);
  ASSERT_EQ(f.fillEvents.size(), 1U);
  f.tick(100, 350'200, 350'400, 0, 100);  // the ask moved: a different level with its own size
  ASSERT_EQ(f.fillEvents.size(), 2U);
  EXPECT_EQ(f.fillEvents[1].priceMills, 350'400);
}

TEST(SimVenue, UnknownSizeCanBeTreatedAsNoLiquidity) {
  SimVenueConfig cfg;
  cfg.sizeZeroMeansUnlimited = false;
  Fixture f(cfg);
  f.tick(0, 350'000, 350'200, 0, 0);  // no displayed size at all
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 100, 350'400)).ok());
  f.at(50);
  EXPECT_TRUE(f.fillEvents.empty());      // unknown liquidity is not evidence of any
  f.tick(100, 350'000, 350'200, 0, 500);  // now the size is known
  EXPECT_EQ(f.fillEvents.size(), 1U);
}

TEST(SimVenue, LegacyUnlimitedBehaviourIsAvailableForHandMadeQuotes) {
  Fixture f;  // default: size 0 means unlimited
  f.tick(0, 350'000, 350'200, 0, 0);
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 500, 350'400)).ok());
  f.at(50);
  ASSERT_EQ(f.fillEvents.size(), 1U);
  EXPECT_EQ(f.fillEvents[0].qty, 500);
}

// --- Cancels and reservations -------------------------------------------------------------------

TEST(SimVenue, CancelRacingAFillCanLose) {
  Fixture f;
  f.tick(0, 350'000, 350'400);
  const auto placed = f.venue.place(order(Side::kBuy, 100, 350'200));
  ASSERT_TRUE(placed.ok());
  f.at(50);
  f.clock.setNs(100 * kMs);
  ASSERT_TRUE(f.venue.cancel(placed.value().orderId).ok());  // effective at 150 ms
  f.tick(120, 349'800, 350'000);  // the market trades through before the cancel lands
  EXPECT_EQ(f.stored(placed.value().orderId).status, 11);
  f.at(200);
  EXPECT_EQ(f.stored(placed.value().orderId).status, 11);  // the late cancel is a no-op
  EXPECT_EQ(f.fillEvents.size(), 1U);
}

TEST(SimVenue, CancelThatLandsFirstStopsTheOrder) {
  Fixture f;
  f.tick(0, 350'000, 350'400);
  const auto placed = f.venue.place(order(Side::kBuy, 100, 350'200));
  ASSERT_TRUE(placed.ok());
  f.at(50);
  ASSERT_TRUE(f.venue.cancel(placed.value().orderId).ok());
  f.at(100);
  EXPECT_EQ(f.stored(placed.value().orderId).status, 15);
  f.tick(200, 349'800, 350'000);  // would have filled, but it is gone
  EXPECT_TRUE(f.fillEvents.empty());
  EXPECT_FALSE(f.venue.cancel(placed.value().orderId).ok());  // already terminal
  EXPECT_FALSE(f.venue.cancel(424242).ok());                  // unknown
}

TEST(SimVenue, RestingBuysReserveBuyingPowerUntilCancelled) {
  Fixture f;
  f.tick(0, 350'000, 350'400);
  const auto first = f.venue.place(order(Side::kBuy, 2500, 350'200));  // ~875,000 HKD reserved
  ASSERT_TRUE(first.ok());
  EXPECT_FALSE(f.venue.place(order(Side::kBuy, 1000, 350'200)).ok());  // only ~125,000 left
  ASSERT_TRUE(f.venue.cancel(first.value().orderId).ok());
  f.at(100);
  EXPECT_TRUE(f.venue.place(order(Side::kBuy, 1000, 350'200)).ok());  // reservation released
}

TEST(SimVenue, RestingSellsReserveShares) {
  Fixture f;
  f.tick(0, 350'000, 350'200);
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 200, 350'200)).ok());
  f.at(50);
  ASSERT_EQ(f.venue.position("00700"), 200);
  const auto sell = f.venue.place(order(Side::kSell, 200, 351'000));
  ASSERT_TRUE(sell.ok());
  EXPECT_FALSE(f.venue.place(order(Side::kSell, 100, 351'000)).ok());  // shares already committed
  ASSERT_TRUE(f.venue.cancel(sell.value().orderId).ok());
  f.at(150);
  EXPECT_TRUE(f.venue.place(order(Side::kSell, 100, 351'000)).ok());
}

TEST(SimVenue, ShortSellingWorksWhenEnabled) {
  SimVenueConfig cfg;
  cfg.allowShort = true;
  Fixture f(cfg);
  f.tick(0, 350'000, 350'200);
  auto req = order(Side::kSell, 100, 349'800);  // marketable: at or below the bid
  req.sellShort = true;
  ASSERT_TRUE(f.venue.place(req).ok());
  f.at(50);
  EXPECT_EQ(f.venue.position("00700"), -100);
  EXPECT_EQ(f.fillEvents.at(0).priceMills, 350'000);  // hits the bid
}

// --- Reporting to the OMS -----------------------------------------------------------------------

TEST(SimVenue, ListingsReflectTruthAndFundsAddUp) {
  Fixture f;
  f.tick(0, 350'000, 350'200);
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 200, 350'400)).ok());
  f.at(50);
  const auto positions = f.venue.listPositions().value();
  ASSERT_EQ(positions.size(), 1U);
  EXPECT_EQ(positions[0].qty, 200);
  EXPECT_EQ(positions[0].canSellQty, 200);
  EXPECT_EQ(positions[0].costPrice, 350'200);
  EXPECT_EQ(f.venue.listFills().value().size(), 1U);
  ASSERT_TRUE(f.venue.place(order(Side::kSell, 100, 352'000)).ok());
  EXPECT_EQ(f.venue.listPositions().value()[0].canSellQty, 100);  // 100 committed to a resting sell

  const auto funds = f.venue.funds().value();
  EXPECT_EQ(funds.cash, f.venue.cash());
  EXPECT_EQ(funds.totalAssets, funds.cash + funds.marketValue);
  EXPECT_EQ(funds.marketValue, 200 * 350'100);  // marked at the mid (350.0 / 350.2)
}

TEST(SimVenue, EquityMarksPositionsAndFallsBackToCost) {
  Fixture f;
  f.tick(0, 350'000, 350'200);
  ASSERT_TRUE(f.venue.place(order(Side::kBuy, 100, 350'200)).ok());
  f.at(50);
  const Money cashAfter = f.venue.cash();
  EXPECT_EQ(f.venue.equity({{"00700", 360'000}}), cashAfter + 100 * 360'000);
  EXPECT_EQ(f.venue.equity({}), cashAfter + 100 * 350'200);  // no mark: valued at cost
}

TEST(SimVenue, IdsAreMonotonicAndDeterministicAcrossRuns) {
  const auto run = [] {
    Fixture f;
    f.tick(0, 350'000, 350'200);
    for (int i = 0; i < 5; ++i) {
      (void)f.venue.place(order(Side::kBuy, 100, 350'200, "r" + std::to_string(i)));
    }
    f.at(50);
    f.tick(100, 350'000, 350'200);
    return std::make_pair(f.orderEvents, f.fillEvents);
  };
  const auto a = run();
  const auto b = run();
  ASSERT_EQ(a.first.size(), b.first.size());
  ASSERT_EQ(a.second.size(), b.second.size());
  for (std::size_t i = 0; i < a.second.size(); ++i) {
    EXPECT_EQ(a.second[i].fillId, b.second[i].fillId);
    EXPECT_EQ(a.second[i].priceMills, b.second[i].priceMills);
    EXPECT_EQ(a.second[i].orderId, b.second[i].orderId);
  }
  for (std::size_t i = 1; i < a.second.size(); ++i) {
    EXPECT_GT(a.second[i].orderId, a.second[i - 1].orderId - 1);
  }
}
