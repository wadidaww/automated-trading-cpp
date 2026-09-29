#include "futu_trader/oms/oms.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <set>
#include <thread>

#include "Common.pb.h"
#include "Trd_UpdateOrder.pb.h"
#include "futu_trader/oms/push_router.hpp"
#include "futu_trader/opend/proto_ids.hpp"

using namespace futu_trader;
using namespace futu_trader::oms;

namespace {

// Scriptable in-memory broker.
class FakeVenue : public IVenue {
 public:
  Result<opend::PlacedOrder> place(const opend::PlaceOrderRequest& request) override {
    placed.push_back(request);
    if (onPlace) {
      return onPlace(request);
    }
    return createOrder(request);
  }
  Result<bool> cancel(std::uint64_t id) override {
    cancelled.push_back(id);
    if (cancelError) {
      return *cancelError;
    }
    return true;
  }
  Result<std::vector<opend::BrokerOrder>> listOrders() override {
    if (onListOrders) {
      onListOrders();
    }
    if (failLists) {
      return Error{ErrorCode::kTimeout, "list timed out"};
    }
    return orders;
  }
  Result<std::vector<opend::BrokerFill>> listFills() override {
    if (failLists) {
      return Error{ErrorCode::kTimeout, "list timed out"};
    }
    return fills;
  }
  Result<std::vector<opend::PositionInfo>> listPositions() override {
    if (failLists) {
      return Error{ErrorCode::kTimeout, "list timed out"};
    }
    ++positionQueries;
    if (!positionScript.empty()) {
      auto next = positionScript.front();
      if (positionScript.size() > 1) {
        positionScript.erase(positionScript.begin());
      }
      return next;
    }
    return positions;
  }
  Result<opend::FundsInfo> funds() override {
    opend::FundsInfo info;
    info.cash = cash;
    return info;
  }

  opend::PlacedOrder createOrder(const opend::PlaceOrderRequest& request) {
    opend::BrokerOrder order;
    order.orderId = nextId++;
    order.code = request.code;
    order.side = request.side;
    order.qty = request.qty;
    order.priceMills = request.priceMills;
    order.status = 5;  // Submitted
    order.remark = request.remark;
    orders.push_back(order);
    return {order.orderId, "EX"};
  }
  opend::BrokerOrder* byRemark(const std::string& remark) {
    for (auto& o : orders) {
      if (o.remark == remark) {
        return &o;
      }
    }
    return nullptr;
  }

  std::function<Result<opend::PlacedOrder>(const opend::PlaceOrderRequest&)> onPlace;
  std::function<void()> onListOrders;
  std::optional<Error> cancelError;
  bool failLists{false};
  std::vector<opend::BrokerOrder> orders;
  std::vector<opend::BrokerFill> fills;
  std::vector<opend::PositionInfo> positions;
  std::vector<std::vector<opend::PositionInfo>> positionScript;  // per-query results
  Money cash{0};
  std::uint64_t nextId{9000};
  std::vector<opend::PlaceOrderRequest> placed;
  std::vector<std::uint64_t> cancelled;
  int positionQueries{0};
};

struct Harness {
  explicit Harness(OmsConfig cfg = defaultConfig(),
                   execution::RateLimitConfig rateCfg = {.maxPerWindow = 100,
                                                         .windowMs = 30'000,
                                                         .reservedForCancels = 10},
                   PreTradeConfig ptCfg = defaultPreTrade(), RiskConfig limits = defaultLimits())
      : rate(rateCfg, clock),
        risk(limits, ptCfg, kill, instruments),
        oms(venue, risk, rate, kill, book, clock, cfg) {}

  static void init(Harness& h) {
    h.instruments.add({"00700", 100});
    h.instruments.add({"09988", 100});
  }
  static OmsConfig defaultConfig() {
    OmsConfig cfg;
    cfg.sessionEpoch = "T1";
    return cfg;
  }
  static PreTradeConfig defaultPreTrade() {
    return {.priceBandBps = 500,
            .maxQuoteAgeMs = 2000,
            .maxOrderNotionalMills = 1'000'000'000,
            .allowShort = false};
  }
  static RiskConfig defaultLimits() {
    return {.maxPositionNotionalMinor = 2'000'000'000,
            .maxPortfolioNotionalMinor = 10'000'000'000,
            .maxDailyLossMinor = 50'000'000,
            .maxOpenOrders = 50,
            .concentrationLimit = 1.0};
  }
  QuoteContext quote(Money last = 350'000) const { return {last, clock.nowNs(), clock.nowNs()}; }
  static OrderIntent buy(const std::string& key, std::int64_t qty = 100, Money price = 350'000) {
    return {key, "00700", Side::kBuy, qty, price};
  }
  static OrderIntent sell(const std::string& key, std::int64_t qty = 100, Money price = 350'000) {
    return {key, "00700", Side::kSell, qty, price};
  }
  static opend::BrokerFill fill(const std::string& id, Side side, std::int64_t qty, Money price,
                                const std::string& code = "00700") {
    opend::BrokerFill f;
    f.fillId = id;
    f.code = code;
    f.side = side;
    f.qty = qty;
    f.priceMills = price;
    return f;
  }
  void boot() {
    init(*this);
    ASSERT_TRUE(oms.bootstrap().ok());
  }

  ManualClock clock;
  execution::KillSwitch kill;
  instrument::InstrumentTable instruments;
  portfolio::PositionBook book;
  FakeVenue venue;
  execution::RateLimiter rate;
  PreTradeRisk risk;
  Oms oms;
};

}  // namespace

// --- Bootstrap ----------------------------------------------------------------------------------

TEST(Oms, RefusesToTradeBeforeBootstrap) {
  Harness h;
  Harness::init(h);
  const auto result = h.oms.submit(Harness::buy("k"), h.quote());
  EXPECT_EQ(result.status, SubmitStatus::kNotReady);
  EXPECT_TRUE(h.venue.placed.empty());
}

TEST(Oms, BootstrapSeedsPositionsAndAdoptsLiveOrders) {
  Harness h;
  Harness::init(h);
  h.venue.positions = {{"00700", 200, 200, 340'000, 350'000}, {"09988", -100, 100, 80'000, 79'000}};
  opend::BrokerOrder live;
  live.orderId = 42;
  live.code = "00700";
  live.qty = 100;
  live.priceMills = 351'000;
  live.status = 5;
  live.remark = "manual-order";
  opend::BrokerOrder done = live;
  done.orderId = 43;
  done.status = 11;
  h.venue.orders = {live, done};
  ASSERT_TRUE(h.oms.bootstrap().ok());
  EXPECT_EQ(h.book.qty("00700"), 200);
  EXPECT_EQ(h.book.qty("09988"), -100);
  EXPECT_EQ(h.oms.liveOrderCount(), 1U);  // the filled one is not adopted
  const auto adopted = h.oms.orders();
  ASSERT_EQ(adopted.size(), 1U);
  EXPECT_TRUE(adopted[0].external);
  EXPECT_EQ(adopted[0].state, OmsState::kWorking);
}

TEST(Oms, BootstrapFailureLeavesTradingDisabled) {
  Harness h;
  Harness::init(h);
  h.venue.failLists = true;
  EXPECT_FALSE(h.oms.bootstrap().ok());
  EXPECT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).status, SubmitStatus::kNotReady);
}

// --- Submit -------------------------------------------------------------------------------------

TEST(Oms, HappyPathPlacesOrderWithClOrdIdInRemark) {
  Harness h;
  h.boot();
  const auto result = h.oms.submit(Harness::buy("k1"), h.quote());
  ASSERT_EQ(result.status, SubmitStatus::kAccepted) << result.detail;
  EXPECT_EQ(result.clOrdId, "FT-T1-1");
  ASSERT_EQ(h.venue.placed.size(), 1U);
  EXPECT_EQ(h.venue.placed[0].remark, "FT-T1-1");
  EXPECT_EQ(h.venue.placed[0].qty, 100);
  EXPECT_EQ(h.venue.placed[0].priceMills, 350'000);
  const auto rec = h.oms.order(result.clOrdId);
  ASSERT_TRUE(rec.has_value());
  EXPECT_EQ(rec->state, OmsState::kWorking);
  EXPECT_EQ(rec->venueOrderId, 9000U);
  EXPECT_EQ(h.oms.liveOrderCount(), 1U);
}

TEST(Oms, RiskRejectNeverReachesVenueAndDoesNotSpendRateBudget) {
  Harness h;
  h.boot();
  const auto bad = h.oms.submit(Harness::buy("k", 150), h.quote());  // lot is 100
  EXPECT_EQ(bad.status, SubmitStatus::kRejectedByRisk);
  EXPECT_EQ(bad.risk, RiskReject::kLotSize);
  EXPECT_TRUE(h.venue.placed.empty());
  EXPECT_EQ(h.rate.used(), 0U);
  EXPECT_EQ(h.oms.liveOrderCount(), 0U);
}

TEST(Oms, RateLimitRejectsWithoutSending) {
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 3, .windowMs = 30'000, .reservedForCancels = 1});
  h.boot();
  EXPECT_EQ(h.oms.submit(Harness::buy("a"), h.quote()).status, SubmitStatus::kAccepted);
  EXPECT_EQ(h.oms.submit(Harness::buy("b"), h.quote()).status, SubmitStatus::kAccepted);
  const auto limited = h.oms.submit(Harness::buy("c"), h.quote());
  EXPECT_EQ(limited.status, SubmitStatus::kRejectedByRisk);
  EXPECT_EQ(limited.risk, RiskReject::kRateLimit);
  EXPECT_EQ(h.venue.placed.size(), 2U);
}

TEST(Oms, SameIntentKeyNeverProducesTwoOrders) {
  Harness h;
  h.boot();
  ASSERT_EQ(h.oms.submit(Harness::buy("dup"), h.quote()).status, SubmitStatus::kAccepted);
  const auto again = h.oms.submit(Harness::buy("dup"), h.quote());
  EXPECT_EQ(again.status, SubmitStatus::kDuplicate);
  EXPECT_EQ(h.venue.placed.size(), 1U);
}

TEST(Oms, MissingIntentKeyIsInvalid) {
  Harness h;
  h.boot();
  EXPECT_EQ(h.oms.submit(Harness::buy(""), h.quote()).status, SubmitStatus::kInvalid);
}

TEST(Oms, DefiniteVenueRejectReleasesTheIntentForRetry) {
  Harness h;
  h.boot();
  h.venue.onPlace = [](const opend::PlaceOrderRequest&) -> Result<opend::PlacedOrder> {
    return Error{ErrorCode::kServer, "insufficient buying power"};
  };
  const auto first = h.oms.submit(Harness::buy("k"), h.quote());
  EXPECT_EQ(first.status, SubmitStatus::kRejectedByVenue);
  EXPECT_EQ(h.oms.order(first.clOrdId)->state, OmsState::kRejected);
  EXPECT_EQ(h.oms.liveOrderCount(), 0U);

  h.venue.onPlace = nullptr;
  const auto retry = h.oms.submit(Harness::buy("k"), h.quote());
  ASSERT_EQ(retry.status, SubmitStatus::kAccepted);
  EXPECT_NE(retry.clOrdId, first.clOrdId);  // a new order id, the rejected one stays as history
}

TEST(Oms, SellingWhatWeDoNotHoldIsBlockedByDefault) {
  Harness h;
  h.boot();
  EXPECT_EQ(h.oms.submit(Harness::sell("s"), h.quote()).risk, RiskReject::kShortSale);
}

TEST(Oms, SellingHeldSharesIsPlacedAsPlainSellNotShort) {
  Harness h;
  Harness::init(h);
  h.venue.positions = {{"00700", 200, 200, 340'000, 350'000}};
  ASSERT_TRUE(h.oms.bootstrap().ok());
  ASSERT_EQ(h.oms.submit(Harness::sell("s", 100), h.quote()).status, SubmitStatus::kAccepted);
  EXPECT_FALSE(h.venue.placed[0].sellShort);
  EXPECT_EQ(h.venue.placed[0].side, Side::kSell);
}

TEST(Oms, ShortingWhenAllowedIsPlacedAsSellShort) {
  PreTradeConfig pt = Harness::defaultPreTrade();
  pt.allowShort = true;
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 100, .windowMs = 30'000, .reservedForCancels = 10}, pt);
  h.boot();
  ASSERT_EQ(h.oms.submit(Harness::sell("s", 100), h.quote()).status, SubmitStatus::kAccepted);
  EXPECT_TRUE(h.venue.placed[0].sellShort);
}

TEST(Oms, RestingOrdersCountTowardsPositionLimitsTogether) {
  // Each order alone fits the 40,000,000-mill (40,000 HKD) cap; three resting ones do not.
  RiskConfig limits = Harness::defaultLimits();
  limits.maxPositionNotionalMinor = 70'000'000;
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 100, .windowMs = 30'000, .reservedForCancels = 10},
            Harness::defaultPreTrade(), limits);
  h.boot();
  ASSERT_EQ(h.oms.submit(Harness::buy("a", 100), h.quote()).status,
            SubmitStatus::kAccepted);  // 35m
  ASSERT_EQ(h.oms.submit(Harness::buy("b", 100), h.quote()).status,
            SubmitStatus::kAccepted);  // 70m
  const auto third = h.oms.submit(Harness::buy("c", 100), h.quote());
  EXPECT_EQ(third.status, SubmitStatus::kRejectedByRisk);
  EXPECT_EQ(third.risk, RiskReject::kPositionLimit);
}

TEST(Oms, DailyLossFromRealizedFillsBlocksNewOrders) {
  RiskConfig limits = Harness::defaultLimits();
  limits.maxDailyLossMinor = 1'000'000;  // 1,000 HKD
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 100, .windowMs = 30'000, .reservedForCancels = 10},
            Harness::defaultPreTrade(), limits);
  h.boot();
  h.oms.onFill(Harness::fill("f1", Side::kBuy, 100, 350'000));
  h.oms.onFill(Harness::fill("f2", Side::kSell, 100, 330'000));  // -2,000 HKD gross, plus fees
  const auto blocked = h.oms.submit(Harness::buy("k", 100, 330'000), h.quote(330'000));
  EXPECT_EQ(blocked.status, SubmitStatus::kRejectedByRisk);
  EXPECT_EQ(blocked.risk, RiskReject::kDailyLoss);
}

TEST(Oms, DailyLossBreachHaltsTradingNotJustTheOneOrder) {
  RiskConfig limits = Harness::defaultLimits();
  limits.maxDailyLossMinor = 1'000'000;
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 100, .windowMs = 30'000, .reservedForCancels = 10},
            Harness::defaultPreTrade(), limits);
  h.boot();
  ASSERT_EQ(h.oms.submit(Harness::buy("rest", 100, 300'000), h.quote(300'000)).status,
            SubmitStatus::kAccepted);  // a resting order that a halt must cancel
  h.oms.onFill(Harness::fill("f1", Side::kBuy, 100, 350'000));
  h.oms.onFill(Harness::fill("f2", Side::kSell, 100, 330'000));
  const auto blocked = h.oms.submit(Harness::buy("k", 100, 330'000), h.quote(330'000));
  EXPECT_EQ(blocked.risk, RiskReject::kDailyLoss);
  EXPECT_TRUE(h.kill.tripped());            // the loss limit is a halt, not a per-order reject
  EXPECT_FALSE(h.venue.cancelled.empty());  // and the resting order was pulled
}

TEST(Oms, DailyBaselineResetPlusHumanResetForgivesYesterdaysLoss) {
  RiskConfig limits = Harness::defaultLimits();
  limits.maxDailyLossMinor = 1'000'000;
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 100, .windowMs = 30'000, .reservedForCancels = 10},
            Harness::defaultPreTrade(), limits);
  h.boot();
  h.oms.onFill(Harness::fill("f1", Side::kBuy, 100, 350'000));
  h.oms.onFill(Harness::fill("f2", Side::kSell, 100, 330'000));
  ASSERT_EQ(h.oms.submit(Harness::buy("k", 100, 330'000), h.quote(330'000)).risk,
            RiskReject::kDailyLoss);
  h.oms.resetDailyBaseline();
  ASSERT_TRUE(h.kill.reset("alice"));  // tripping needs an explicit human reset
  EXPECT_EQ(h.oms.submit(Harness::buy("k2", 100, 330'000), h.quote(330'000)).status,
            SubmitStatus::kAccepted);
}

TEST(Oms, SeededUnrealisedGainIsNotACushionAgainstLaterLosses) {
  // Carried position: 100 @ 340 cost, marked 350 (+1,000,000 mills unrealised). Loss cap 500,000.
  // Selling at 335 realises -500,000 vs cost, but from the start-of-day value (350) the day is
  // down 1,500,000. With a zero baseline the gain would have hidden this loss.
  RiskConfig limits = Harness::defaultLimits();
  limits.maxDailyLossMinor = 500'000;
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 100, .windowMs = 30'000, .reservedForCancels = 10},
            Harness::defaultPreTrade(), limits);
  Harness::init(h);
  h.venue.positions = {{"00700", 100, 100, 340'000, 350'000}};
  ASSERT_TRUE(h.oms.bootstrap().ok());
  h.oms.onFill(Harness::fill("s1", Side::kSell, 100, 335'000));
  EXPECT_EQ(h.oms.submit(Harness::buy("k", 100, 335'000), h.quote(335'000)).risk,
            RiskReject::kDailyLoss);
}

// --- Ambiguity ----------------------------------------------------------------------------------

TEST(Oms, TimedOutSubmitBecomesUnknownAndBlocksTheIntentAndSymbol) {
  Harness h;
  h.boot();
  h.venue.onPlace = [&](const opend::PlaceOrderRequest& req) -> Result<opend::PlacedOrder> {
    h.venue.createOrder(req);                        // the order DOES exist at the broker...
    return Error{ErrorCode::kTimeout, "timed out"};  // ...but we never heard back
  };
  const auto result = h.oms.submit(Harness::buy("k"), h.quote());
  EXPECT_EQ(result.status, SubmitStatus::kAmbiguous);
  EXPECT_EQ(h.oms.order(result.clOrdId)->state, OmsState::kUnknown);
  EXPECT_EQ(h.oms.unresolvedCount(), 1U);
  EXPECT_EQ(h.oms.liveOrderCount(), 1U);  // an unknown order must count against risk

  h.venue.onPlace = nullptr;
  EXPECT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).status, SubmitStatus::kBlockedUnresolved);
  const auto other = h.oms.submit(Harness::buy("other"), h.quote());
  EXPECT_EQ(other.status, SubmitStatus::kRejectedByRisk);
  EXPECT_EQ(other.risk, RiskReject::kUnresolvedOrder);
  EXPECT_EQ(h.venue.orders.size(), 1U);  // and crucially: no second order was created
}

TEST(Oms, CannotCancelAnUnresolvedOrder) {
  Harness h;
  h.boot();
  h.venue.onPlace = [](const opend::PlaceOrderRequest&) -> Result<opend::PlacedOrder> {
    return Error{ErrorCode::kDisconnected, "link down"};
  };
  const auto result = h.oms.submit(Harness::buy("k"), h.quote());
  ASSERT_EQ(result.status, SubmitStatus::kAmbiguous);
  EXPECT_FALSE(h.oms.cancel(result.clOrdId).ok());
  EXPECT_TRUE(h.venue.cancelled.empty());
}

TEST(Oms, ReconcileFindsTheAmbiguousOrderByRemarkAndAdoptsIt) {
  Harness h;
  h.boot();
  h.venue.onPlace = [&](const opend::PlaceOrderRequest& req) -> Result<opend::PlacedOrder> {
    h.venue.createOrder(req);
    return Error{ErrorCode::kTimeout, "timed out"};
  };
  const auto result = h.oms.submit(Harness::buy("k"), h.quote());
  ASSERT_EQ(result.status, SubmitStatus::kAmbiguous);
  h.venue.onPlace = nullptr;

  const auto report = h.oms.reconcile();
  ASSERT_TRUE(report.complete);
  EXPECT_EQ(report.resolvedUnknown, 1U);
  EXPECT_TRUE(report.drifts.empty());
  const auto rec = h.oms.order(result.clOrdId);
  EXPECT_EQ(rec->state, OmsState::kWorking);
  EXPECT_EQ(rec->venueOrderId, 9000U);
  // The intent is now known to have produced an order: it must not be resent.
  EXPECT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).status, SubmitStatus::kDuplicate);
  EXPECT_EQ(h.venue.orders.size(), 1U);
}

TEST(Oms, AmbiguousOrderNotListedNeedsGraceAndSeveralConsecutiveListings) {
  Harness h;  // grace 3 s, 3 consecutive listings
  h.boot();
  h.venue.onPlace = [](const opend::PlaceOrderRequest&) -> Result<opend::PlacedOrder> {
    return Error{ErrorCode::kTimeout, "timed out"};  // the order really never reached the broker
  };
  const auto result = h.oms.submit(Harness::buy("k"), h.quote());
  ASSERT_EQ(result.status, SubmitStatus::kAmbiguous);

  h.clock.advanceMs(1000);  // inside the grace period: the broker's list may simply be behind
  EXPECT_EQ(h.oms.reconcile().resolvedUnknown, 0U);
  h.clock.advanceMs(2500);  // grace over, but this is only the 2nd listing without it
  EXPECT_EQ(h.oms.reconcile().resolvedUnknown, 0U);
  EXPECT_EQ(h.oms.order(result.clOrdId)->state, OmsState::kUnknown);

  const auto third = h.oms.reconcile();  // 3rd consecutive listing without it
  EXPECT_EQ(third.resolvedUnknown, 1U);
  EXPECT_EQ(h.oms.order(result.clOrdId)->state, OmsState::kRejected);
  EXPECT_TRUE(third.drifts.empty());
}

TEST(Oms, PresumedDeadIntentStaysBlockedUnderItsKeyButANewIntentIsFine) {
  OmsConfig cfg = Harness::defaultConfig();
  cfg.absenceListingsRequired = 1;
  cfg.ambiguityGraceMs = 0;
  Harness h(cfg);
  h.boot();
  h.venue.onPlace = [](const opend::PlaceOrderRequest&) -> Result<opend::PlacedOrder> {
    return Error{ErrorCode::kTimeout, "timed out"};
  };
  ASSERT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).status, SubmitStatus::kAmbiguous);
  h.clock.advanceMs(1);
  ASSERT_EQ(h.oms.reconcile().resolvedUnknown, 1U);
  h.venue.onPlace = nullptr;
  // The same key must not silently place a second order: the first one may still surface.
  EXPECT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).status, SubmitStatus::kDuplicate);
  EXPECT_EQ(h.oms.submit(Harness::buy("k-new"), h.quote()).status, SubmitStatus::kAccepted);
  EXPECT_EQ(h.venue.orders.size(), 1U);
}

TEST(Oms, LateArrivalOfAPresumedDeadOrderHaltsAdoptsAndCancelsIt) {
  // Review finding: after "never existed" was decided and the intent retried, the original order
  // reaches the broker anyway. It must not become an untracked live order.
  OmsConfig cfg = Harness::defaultConfig();
  cfg.absenceListingsRequired = 1;
  cfg.ambiguityGraceMs = 0;
  Harness h(cfg);
  h.boot();
  h.venue.onPlace = [](const opend::PlaceOrderRequest&) -> Result<opend::PlacedOrder> {
    return Error{ErrorCode::kTimeout, "timed out"};
  };
  const auto ambiguous = h.oms.submit(Harness::buy("k"), h.quote());
  h.clock.advanceMs(1);
  ASSERT_EQ(h.oms.reconcile().resolvedUnknown, 1U);
  ASSERT_EQ(h.oms.order(ambiguous.clOrdId)->state, OmsState::kRejected);

  h.venue.onPlace = nullptr;
  opend::BrokerOrder late;  // ...and now the broker lists it as live, carrying our remark
  late.orderId = 4242;
  late.code = "00700";
  late.qty = 100;
  late.priceMills = 350'000;
  late.status = 5;
  late.remark = ambiguous.clOrdId;
  h.venue.orders.push_back(late);

  const auto report = h.oms.reconcile();
  EXPECT_TRUE(h.kill.tripped());
  EXPECT_TRUE(report.halted);
  const auto adopted = h.oms.order("LATE-4242");
  ASSERT_TRUE(adopted.has_value());  // tracked, so exposure counts it and the halt can cancel it
  EXPECT_TRUE(adopted->external);
  ASSERT_FALSE(h.venue.cancelled.empty());
  EXPECT_EQ(h.venue.cancelled[0], 4242U);
}

TEST(Oms, LateArrivalReportedByPushAlsoHalts) {
  OmsConfig cfg = Harness::defaultConfig();
  cfg.absenceListingsRequired = 1;
  cfg.ambiguityGraceMs = 0;
  Harness h(cfg);
  h.boot();
  h.venue.onPlace = [](const opend::PlaceOrderRequest&) -> Result<opend::PlacedOrder> {
    return Error{ErrorCode::kTimeout, "timed out"};
  };
  const auto ambiguous = h.oms.submit(Harness::buy("k"), h.quote());
  h.clock.advanceMs(1);
  ASSERT_EQ(h.oms.reconcile().resolvedUnknown, 1U);
  opend::BrokerOrder late;
  late.orderId = 77;
  late.code = "00700";
  late.qty = 100;
  late.status = 10;
  late.fillQty = 100;
  late.remark = ambiguous.clOrdId;
  h.oms.onOrderUpdate(late);
  EXPECT_TRUE(h.kill.tripped());
  EXPECT_TRUE(h.oms.haltPending());
}

TEST(Oms, PresumedDeadOrderShowingAsRejectedIsBenign) {
  OmsConfig cfg = Harness::defaultConfig();
  cfg.absenceListingsRequired = 1;
  cfg.ambiguityGraceMs = 0;
  Harness h(cfg);
  h.boot();
  h.venue.onPlace = [](const opend::PlaceOrderRequest&) -> Result<opend::PlacedOrder> {
    return Error{ErrorCode::kTimeout, "timed out"};
  };
  const auto ambiguous = h.oms.submit(Harness::buy("k"), h.quote());
  h.clock.advanceMs(1);
  ASSERT_EQ(h.oms.reconcile().resolvedUnknown, 1U);
  opend::BrokerOrder confirm;
  confirm.orderId = 78;
  confirm.code = "00700";
  confirm.qty = 100;
  confirm.status = 21;  // Failed: consistent with "never existed"
  confirm.remark = ambiguous.clOrdId;
  h.oms.onOrderUpdate(confirm);
  EXPECT_FALSE(h.kill.tripped());
}

TEST(Oms, ReconcileSkipsOrdersSentAfterTheListWasFetched) {
  // An order placed while the list request is in flight is legitimately missing from that list.
  Harness h;
  h.boot();
  bool fired = false;
  h.venue.onListOrders = [&] {
    if (!fired) {
      fired = true;
      h.clock.advanceMs(5);
      ASSERT_EQ(h.oms.submit(Harness::buy("during"), h.quote()).status, SubmitStatus::kAccepted);
      h.venue.orders.clear();  // the list we are about to return predates that order
    }
  };
  const auto report = h.oms.reconcile();
  EXPECT_TRUE(report.drifts.empty());
  EXPECT_FALSE(h.kill.tripped());
}

// --- Broker events ------------------------------------------------------------------------------

TEST(Oms, OrderUpdatesDriveTheStateMachine) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k", 200), h.quote()).clOrdId;
  auto broker = *h.venue.byRemark(id);
  broker.status = 10;  // partially filled
  broker.fillQty = 100;
  h.oms.onOrderUpdate(broker);
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kPartiallyFilled);
  EXPECT_EQ(h.oms.order(id)->filledQty, 100);
  broker.status = 11;
  broker.fillQty = 200;
  h.oms.onOrderUpdate(broker);
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kFilled);
  EXPECT_EQ(h.oms.liveOrderCount(), 0U);
}

TEST(Oms, StaleAndDuplicateUpdatesAreIgnoredAndCounted) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k", 200), h.quote()).clOrdId;
  auto broker = *h.venue.byRemark(id);
  broker.status = 10;
  broker.fillQty = 100;
  h.oms.onOrderUpdate(broker);
  broker.status = 5;  // a late "submitted" after "partially filled"
  h.oms.onOrderUpdate(broker);
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kPartiallyFilled);
  EXPECT_GE(h.oms.anomalyCount(), 1U);
  broker.status = 10;
  h.oms.onOrderUpdate(broker);  // exact duplicate: no state change, no crash
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kPartiallyFilled);
}

TEST(Oms, TerminalOrderIsNeverResurrected) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  auto broker = *h.venue.byRemark(id);
  broker.status = 11;
  broker.fillQty = 100;
  h.oms.onOrderUpdate(broker);
  ASSERT_EQ(h.oms.order(id)->state, OmsState::kFilled);
  broker.status = 5;
  broker.fillQty = 0;
  h.oms.onOrderUpdate(broker);
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kFilled);
  EXPECT_EQ(h.oms.order(id)->filledQty, 100);  // fills never go backwards
}

TEST(Oms, UnrecognisedBrokerStatusDoesNotChangeStateOrCrash) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  auto broker = *h.venue.byRemark(id);
  broker.status = 9999;
  h.oms.onOrderUpdate(broker);
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kWorking);
  EXPECT_GE(h.oms.anomalyCount(), 1U);
}

TEST(Oms, FillsUpdatePositionsWithFeesAndAreIdempotent) {
  Harness h;
  h.boot();
  h.oms.onFill(Harness::fill("F1", Side::kBuy, 200, 350'200));
  h.oms.onFill(Harness::fill("F1", Side::kBuy, 200, 350'200));  // push replayed
  EXPECT_EQ(h.book.qty("00700"), 200);
  EXPECT_EQ(h.book.totalFees(), 78'960);  // hand-computed golden value from the fee tests
}

TEST(Oms, BustedFillHaltsTradingAndIsNotApplied) {
  Harness h;
  h.boot();
  auto busted = Harness::fill("F9", Side::kBuy, 100, 350'000);
  busted.status = 1;  // OrderFillStatus_Cancelled
  h.oms.onFill(busted);
  EXPECT_TRUE(h.kill.tripped());
  EXPECT_EQ(h.book.qty("00700"), 0);
  EXPECT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).risk, RiskReject::kKillSwitch);
}

TEST(Oms, PushArrivingBeforeThePlaceResponseIsNotOverwritten) {
  // The broker's "filled" push can beat the place() response. The venue is called with no lock
  // held, so the handler may run re-entrantly; the later ack must not drag the order backwards.
  Harness h;
  h.boot();
  h.venue.onPlace = [&](const opend::PlaceOrderRequest& req) -> Result<opend::PlacedOrder> {
    const auto placed = h.venue.createOrder(req);
    auto order = *h.venue.byRemark(req.remark);
    order.status = 11;
    order.fillQty = req.qty;
    h.oms.onOrderUpdate(order);  // push handled while place() is still "in flight"
    h.oms.onFill(Harness::fill("F1", req.side, req.qty, req.priceMills));
    return placed;
  };
  const auto result = h.oms.submit(Harness::buy("k"), h.quote());
  ASSERT_EQ(result.status, SubmitStatus::kAccepted);
  EXPECT_EQ(h.oms.order(result.clOrdId)->state, OmsState::kFilled);
  EXPECT_EQ(h.book.qty("00700"), 100);
}

// --- Cancel and halt ----------------------------------------------------------------------------

TEST(Oms, CancelFlowEndsCancelledOnBrokerConfirmation) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  ASSERT_TRUE(h.oms.cancel(id).ok());
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kCancelPending);
  ASSERT_EQ(h.venue.cancelled.size(), 1U);
  EXPECT_EQ(h.venue.cancelled[0], 9000U);
  auto broker = *h.venue.byRemark(id);
  broker.status = 15;
  h.oms.onOrderUpdate(broker);
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kCancelled);
  EXPECT_FALSE(h.oms.cancel(id).ok());  // no longer live
}

TEST(Oms, RefusedCancelReturnsOrderToWorking) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  h.venue.cancelError = Error{ErrorCode::kServer, "order status does not allow cancel"};
  EXPECT_FALSE(h.oms.cancel(id).ok());
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kWorking);
}

TEST(Oms, RefusedCancelOfAPartiallyFilledOrderRestoresPartialFill) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k", 200), h.quote()).clOrdId;
  auto broker = *h.venue.byRemark(id);
  broker.status = 10;
  broker.fillQty = 100;
  h.oms.onOrderUpdate(broker);
  h.venue.cancelError = Error{ErrorCode::kServer, "refused"};
  EXPECT_FALSE(h.oms.cancel(id).ok());
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kPartiallyFilled);
}

TEST(Oms, AmbiguousCancelStaysPendingUntilTheBrokerSaysOtherwise) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  h.venue.cancelError = Error{ErrorCode::kTimeout, "timed out"};
  EXPECT_FALSE(h.oms.cancel(id).ok());
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kCancelPending);
  auto* broker = h.venue.byRemark(id);
  broker->status = 15;  // the cancel actually went through
  const auto report = h.oms.reconcile();
  EXPECT_EQ(report.statusCatchUps, 1U);
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kCancelled);
}

TEST(Oms, HaltCancelsEverythingUsingTheReservedBudgetEvenWhenNewOrdersAreThrottled) {
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 5, .windowMs = 30'000, .reservedForCancels = 3});
  h.boot();
  ASSERT_EQ(h.oms.submit(Harness::buy("a"), h.quote()).status, SubmitStatus::kAccepted);
  ASSERT_EQ(h.oms.submit(Harness::buy("b"), h.quote()).status, SubmitStatus::kAccepted);
  ASSERT_EQ(h.oms.submit(Harness::buy("c"), h.quote()).risk, RiskReject::kRateLimit);  // exhausted

  const auto report = h.oms.haltAndCancelAll("test halt");
  EXPECT_TRUE(h.kill.tripped());
  EXPECT_EQ(report.cancelRequested, 2U);
  EXPECT_EQ(report.cancelFailed, 0U);
  EXPECT_EQ(h.venue.cancelled.size(), 2U);
  EXPECT_EQ(h.oms.submit(Harness::buy("d"), h.quote()).risk, RiskReject::kKillSwitch);
}

TEST(Oms, HaltReportsOrdersItCannotCancel) {
  Harness h;
  h.boot();
  h.venue.onPlace = [](const opend::PlaceOrderRequest&) -> Result<opend::PlacedOrder> {
    return Error{ErrorCode::kTimeout, "timed out"};
  };
  ASSERT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).status, SubmitStatus::kAmbiguous);
  const auto report = h.oms.haltAndCancelAll("halt");
  EXPECT_EQ(report.unresolvedWithoutVenueId, 1U);  // a human must look at this one
}

// --- Reconciliation -----------------------------------------------------------------------------

TEST(Oms, ReconcileAppliesFillsThePushesNeverDelivered) {
  Harness h;
  h.boot();
  ASSERT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).status, SubmitStatus::kAccepted);
  h.venue.fills = {Harness::fill("F1", Side::kBuy, 100, 350'000)};
  h.venue.positions = {{"00700", 100, 100, 350'000, 350'000}};
  auto* order = h.venue.byRemark("FT-T1-1");
  order->status = 11;
  order->fillQty = 100;
  const auto report = h.oms.reconcile();
  ASSERT_TRUE(report.complete);
  EXPECT_EQ(report.missedFillsApplied, 1U);
  EXPECT_EQ(report.statusCatchUps, 1U);
  EXPECT_TRUE(report.drifts.empty());
  EXPECT_FALSE(h.kill.tripped());
  EXPECT_EQ(h.book.qty("00700"), 100);
  EXPECT_EQ(h.oms.order("FT-T1-1")->state, OmsState::kFilled);
}

TEST(Oms, PositionMismatchHaltsTrading) {
  Harness h;
  h.boot();
  h.venue.positions = {{"00700", 300, 300, 350'000, 350'000}};  // a phantom position at the broker
  const auto report = h.oms.reconcile();
  ASSERT_TRUE(report.complete);
  ASSERT_EQ(report.drifts.size(), 1U);
  EXPECT_EQ(report.drifts[0].kind, DriftKind::kPositionMismatch);
  EXPECT_TRUE(report.halted);
  EXPECT_TRUE(h.kill.tripped());
  EXPECT_FALSE(report.clean());
  EXPECT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).risk, RiskReject::kKillSwitch);
}

TEST(Oms, MissingBrokerPositionIsAlsoAMismatch) {
  Harness h;
  Harness::init(h);
  h.venue.positions = {{"00700", 200, 200, 340'000, 350'000}};
  ASSERT_TRUE(h.oms.bootstrap().ok());
  h.venue.positions.clear();  // broker now says flat, we think we hold 200
  const auto report = h.oms.reconcile();
  ASSERT_EQ(report.drifts.size(), 1U);
  EXPECT_TRUE(h.kill.tripped());
}

TEST(Oms, TransientPositionLagSelfHealsOnTheSecondLook) {
  // A fill lands between our fill query and our position query: the first comparison disagrees,
  // the re-check agrees. That is lag, not drift, and must not halt trading.
  Harness h;
  h.boot();
  h.venue.fills = {Harness::fill("F1", Side::kBuy, 100, 350'000)};
  h.venue.positionScript = {{}, {{"00700", 100, 100, 350'000, 350'000}}};
  h.venue.positionQueries = 0;  // ignore the bootstrap query
  const auto report = h.oms.reconcile();
  EXPECT_TRUE(report.complete);
  EXPECT_TRUE(report.drifts.empty());
  EXPECT_FALSE(h.kill.tripped());
  EXPECT_EQ(h.venue.positionQueries, 2);
}

TEST(Oms, WorkingOrderVanishingFromTheBrokerHaltsTrading) {
  Harness h;
  h.boot();
  ASSERT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).status, SubmitStatus::kAccepted);
  h.venue.orders.clear();  // the broker no longer lists an order we believe is working
  const auto report = h.oms.reconcile();
  ASSERT_EQ(report.drifts.size(), 1U);
  EXPECT_EQ(report.drifts[0].kind, DriftKind::kOrderMissingAtBroker);
  EXPECT_TRUE(h.kill.tripped());
}

TEST(Oms, BrokerReportingAnUnclearStatusIsFlaggedAsDrift) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  h.venue.byRemark(id)->status = 24;  // FillCancelled
  const auto report = h.oms.reconcile();
  ASSERT_FALSE(report.drifts.empty());
  EXPECT_EQ(report.drifts[0].kind, DriftKind::kOrderStatusUnknown);
  EXPECT_TRUE(h.kill.tripped());
}

TEST(Oms, ForeignLiveOrderIsAdoptedAndCountsAgainstRiskButDoesNotHalt) {
  Harness h;
  h.boot();
  opend::BrokerOrder stranger;
  stranger.orderId = 777;
  stranger.code = "00700";
  stranger.qty = 100;
  stranger.priceMills = 350'000;
  stranger.status = 5;
  stranger.remark = "placed-by-hand";
  h.venue.orders = {stranger};
  const auto report = h.oms.reconcile();
  EXPECT_EQ(report.adoptedExternal, 1U);
  EXPECT_TRUE(report.drifts.empty());
  EXPECT_EQ(h.oms.liveOrderCount(), 1U);
  EXPECT_TRUE(h.oms.orders()[0].external);
}

TEST(Oms, RestartRecoversOwnOrdersFromTheirRemark) {
  // A previous process placed FT-OLD-1. After a restart the new OMS adopts it and, because the
  // remark carries our prefix, it is NOT treated as external.
  Harness h;
  Harness::init(h);
  opend::BrokerOrder mine;
  mine.orderId = 555;
  mine.code = "00700";
  mine.qty = 100;
  mine.priceMills = 350'000;
  mine.status = 5;
  mine.remark = "FT-OLD-1";
  h.venue.orders = {mine};
  ASSERT_TRUE(h.oms.bootstrap().ok());
  const auto rec = h.oms.order("FT-OLD-1");
  ASSERT_TRUE(rec.has_value());
  EXPECT_FALSE(rec->external);
  EXPECT_EQ(rec->venueOrderId, 555U);
  EXPECT_TRUE(h.oms.reconcile().clean());
}

TEST(Oms, FailedReconcilesAreIncompleteNotCleanAndEscalateAfterRepeats) {
  Harness h;
  h.boot();
  h.venue.failLists = true;
  auto report = h.oms.reconcile();
  EXPECT_FALSE(report.complete);
  EXPECT_FALSE(report.clean());
  EXPECT_FALSE(report.halted);
  report = h.oms.reconcile();
  EXPECT_FALSE(report.halted);
  report = h.oms.reconcile();  // third consecutive failure
  EXPECT_TRUE(report.halted);
  EXPECT_TRUE(h.kill.tripped());
}

TEST(Oms, SuccessfulReconcileResetsTheFailureStreak) {
  Harness h;
  h.boot();
  h.venue.failLists = true;
  h.oms.reconcile();
  h.oms.reconcile();
  h.venue.failLists = false;
  EXPECT_TRUE(h.oms.reconcile().clean());
  h.venue.failLists = true;
  h.oms.reconcile();
  h.oms.reconcile();
  EXPECT_FALSE(h.kill.tripped());  // streak restarted, so two failures are still tolerated
}

TEST(Oms, CashCheckIsOptInAndCatchesMismatch) {
  OmsConfig cfg = Harness::defaultConfig();
  cfg.cashToleranceMills = 1'000;
  Harness h(cfg);
  Harness::init(h);
  h.venue.cash = 1'000'000'000;
  ASSERT_TRUE(h.oms.bootstrap().ok());
  EXPECT_TRUE(h.oms.reconcile().clean());  // nothing traded, cash unchanged

  h.venue.cash = 999'000'000;  // 1,000 HKD vanished without a fill
  const auto report = h.oms.reconcile();
  ASSERT_EQ(report.drifts.size(), 1U);
  EXPECT_EQ(report.drifts[0].kind, DriftKind::kCashMismatch);
  EXPECT_TRUE(h.kill.tripped());
}

// --- Journal and concurrency --------------------------------------------------------------------

TEST(Oms, JournalRecordsTheLifecycleAndFeedsTheSink) {
  Harness h;
  h.boot();
  std::vector<JournalEntry> sunk;
  h.oms.setJournalSink([&](const JournalEntry& e) { sunk.push_back(e); });
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  const auto journal = h.oms.journal();
  bool sawRequest = false;
  bool sawPlaced = false;
  for (const auto& entry : journal) {
    sawRequest = sawRequest || (entry.kind == JournalKind::kSubmitRequested && entry.clOrdId == id);
    sawPlaced = sawPlaced || (entry.kind == JournalKind::kPlaced && entry.clOrdId == id);
  }
  EXPECT_TRUE(sawRequest);
  EXPECT_TRUE(sawPlaced);
  EXPECT_FALSE(sunk.empty());
}

TEST(Oms, ConcurrentSubmitPushAndReconcileAreRaceFreeAndConsistent) {
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 100000, .windowMs = 30'000, .reservedForCancels = 10},
            Harness::defaultPreTrade(),
            {.maxPositionNotionalMinor = INT64_MAX / 4,
             .maxPortfolioNotionalMinor = INT64_MAX / 4,
             .maxDailyLossMinor = INT64_MAX / 4,
             .maxOpenOrders = 100000,
             .concentrationLimit = 1.0});
  h.boot();
  std::atomic<bool> done{false};
  std::atomic<int> accepted{0};
  std::atomic<int> pushed{0};

  std::thread pusher([&] {
    int n = 0;
    while (!done.load()) {
      opend::BrokerOrder order;
      order.orderId = 9000 + static_cast<std::uint64_t>(n % 50);
      order.remark = "FT-T1-" + std::to_string(1 + (n % 50));
      order.status = 10;
      order.fillQty = 100;
      order.qty = 100;
      h.oms.onOrderUpdate(order);
      h.oms.onFill(Harness::fill("P" + std::to_string(n), Side::kBuy, 100, 350'000));
      ++pushed;
      ++n;
    }
  });
  std::thread reader([&] {
    while (!done.load()) {
      (void)h.oms.orders();
      (void)h.oms.liveOrderCount();
      (void)h.oms.journal();
    }
  });
  for (int i = 0; i < 200; ++i) {
    const auto result = h.oms.submit(Harness::buy("c" + std::to_string(i)), h.quote());
    if (result.status == SubmitStatus::kAccepted) {
      ++accepted;
    }
  }
  done = true;
  pusher.join();
  reader.join();
  EXPECT_EQ(accepted.load(), 200);
  EXPECT_EQ(h.venue.placed.size(), 200U);
  // Every distinct fill was applied exactly once, whatever the interleaving with submits.
  EXPECT_EQ(h.book.qty("00700"), 100 * static_cast<std::int64_t>(pushed.load()));
  // Every ClOrdId is unique.
  std::set<std::string> ids;
  for (const auto& rec : h.oms.orders()) {
    ids.insert(rec.clOrdId);
  }
  EXPECT_EQ(ids.size(), h.oms.orders().size());
}

// --- Cancel retry, halt completion, restart, accounting (review findings) -----------------------

TEST(Oms, UnconfirmedCancelIsResentAfterTheRetryWindowButNotBefore) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  h.venue.cancelError = Error{ErrorCode::kTimeout, "timed out"};
  EXPECT_FALSE(h.oms.cancel(id).ok());  // first attempt never reached the broker
  ASSERT_EQ(h.venue.cancelled.size(), 1U);

  h.venue.cancelError.reset();
  EXPECT_TRUE(h.oms.cancel(id).ok());  // too soon: not resent, and not falsely "done"
  EXPECT_EQ(h.venue.cancelled.size(), 1U);
  h.clock.advanceMs(2500);
  EXPECT_TRUE(h.oms.cancel(id).ok());  // past the window: resent for real
  EXPECT_EQ(h.venue.cancelled.size(), 2U);
}

TEST(Oms, HaltAlwaysResendsCancelsEvenIfOneIsAlreadyPending) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  h.venue.cancelError = Error{ErrorCode::kTimeout, "timed out"};
  EXPECT_FALSE(h.oms.cancel(id).ok());
  h.venue.cancelError.reset();
  h.oms.haltAndCancelAll("halt now");  // no waiting for the retry window
  EXPECT_EQ(h.venue.cancelled.size(), 2U);
}

TEST(Oms, CancelPendingWhileBrokerStillShowsWorkingIsUndoneSoItCanBeResent) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  h.venue.cancelError = Error{ErrorCode::kTimeout, "timed out"};
  EXPECT_FALSE(h.oms.cancel(id).ok());
  ASSERT_EQ(h.oms.order(id)->state, OmsState::kCancelPending);
  h.clock.advanceMs(2500);
  const auto report = h.oms.reconcile();  // broker still says Submitted
  EXPECT_EQ(report.statusCatchUps, 1U);
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kWorking);
  h.venue.cancelError.reset();
  EXPECT_TRUE(h.oms.cancel(id).ok());
  EXPECT_EQ(h.oms.order(id)->state, OmsState::kCancelPending);
  EXPECT_EQ(h.venue.cancelled.size(), 2U);
}

TEST(Oms, HaltBeyondTheCancelReserveFinishesInLaterServiceCalls) {
  // 5 resting orders but only 3 cancels of budget: the rest must not be silently abandoned.
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 8, .windowMs = 30'000, .reservedForCancels = 3});
  h.boot();
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(h.oms.submit(Harness::buy("o" + std::to_string(i)), h.quote()).status,
              SubmitStatus::kAccepted);
  }
  const auto first = h.oms.haltAndCancelAll("drill");
  EXPECT_EQ(first.cancelRequested, 5U);
  EXPECT_EQ(first.cancelFailed, 2U);  // out of budget
  EXPECT_EQ(h.venue.cancelled.size(), 3U);
  EXPECT_TRUE(h.oms.haltPending());

  h.clock.advanceMs(31'000);  // the rate window frees up
  h.oms.serviceHalt();
  // Every order has now had a cancel sent (the three unconfirmed ones are resent after the
  // retry window, the two that never got budget go out for the first time).
  std::set<std::uint64_t> distinct(h.venue.cancelled.begin(), h.venue.cancelled.end());
  EXPECT_EQ(distinct.size(), 5U);
}

TEST(Oms, HaltPendingClearsOnceNothingLiveRemains) {
  Harness h;
  h.boot();
  const auto id = h.oms.submit(Harness::buy("k"), h.quote()).clOrdId;
  h.oms.haltAndCancelAll("drill");
  EXPECT_TRUE(h.oms.haltPending());  // cancel requested, not yet confirmed
  auto broker = *h.venue.byRemark(id);
  broker.status = 15;
  h.oms.onOrderUpdate(broker);
  h.oms.serviceHalt();
  EXPECT_FALSE(h.oms.haltPending());
}

TEST(Oms, ReconciliationDriftCancelsRestingOrdersNotJustBlocksNewOnes) {
  Harness h;
  h.boot();
  ASSERT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).status, SubmitStatus::kAccepted);
  h.venue.positions = {{"00700", 900, 900, 350'000, 350'000}};  // unexplained position
  const auto report = h.oms.reconcile();
  EXPECT_TRUE(report.halted);
  EXPECT_EQ(h.venue.cancelled.size(), 1U);  // the resting order was pulled by the halt
}

TEST(Oms, BustedFillHaltSchedulesCancelAllForTheEngineThread) {
  Harness h;
  h.boot();
  ASSERT_EQ(h.oms.submit(Harness::buy("k"), h.quote()).status, SubmitStatus::kAccepted);
  auto busted = Harness::fill("F9", Side::kBuy, 100, 350'000);
  busted.status = 1;
  h.oms.onFill(busted);  // runs on the push thread: it must NOT call the venue
  EXPECT_TRUE(h.venue.cancelled.empty());
  EXPECT_TRUE(h.oms.haltPending());
  h.oms.serviceHalt();  // the engine thread performs the cancels
  EXPECT_EQ(h.venue.cancelled.size(), 1U);
}

TEST(Oms, HaltDuringPlaceStillCancelsTheOrderThatThenGetsAccepted) {
  Harness h;
  h.boot();
  h.venue.onPlace = [&](const opend::PlaceOrderRequest& req) -> Result<opend::PlacedOrder> {
    h.kill.trip("operator pressed the button");  // halt lands while place() is in flight
    return h.venue.createOrder(req);
  };
  const auto result = h.oms.submit(Harness::buy("k"), h.quote());
  ASSERT_EQ(result.status, SubmitStatus::kAccepted);
  ASSERT_EQ(h.venue.cancelled.size(), 1U);  // it was cancelled straight away
  EXPECT_EQ(h.venue.cancelled[0], 9000U);
}

TEST(Oms, BootstrapWithTodaysFillsDoesNotDoubleCountThemOnFirstReconcile) {
  // Review finding: an intraday restart seeds the broker position, which already contains today's
  // fills; the first reconcile must not apply them again.
  Harness h;
  Harness::init(h);
  h.venue.positions = {{"00700", 100, 100, 350'000, 350'000}};
  h.venue.fills = {Harness::fill("EARLIER-1", Side::kBuy, 100, 350'000)};
  ASSERT_TRUE(h.oms.bootstrap().ok());
  const auto report = h.oms.reconcile();
  EXPECT_TRUE(report.clean()) << (report.drifts.empty() ? "" : report.drifts[0].detail);
  EXPECT_EQ(report.missedFillsApplied, 0U);
  EXPECT_EQ(h.book.qty("00700"), 100);
  EXPECT_FALSE(h.kill.tripped());
  // A genuinely new fill is still applied.
  h.venue.fills.push_back(Harness::fill("NEW-1", Side::kBuy, 100, 351'000));
  h.venue.positions = {{"00700", 200, 200, 350'500, 351'000}};
  EXPECT_TRUE(h.oms.reconcile().clean());
  EXPECT_EQ(h.book.qty("00700"), 200);
}

TEST(Oms, QuoteFreshnessUsesTheOmsClockNotTheCallersClaim) {
  Harness h;
  h.boot();
  h.clock.advanceMs(60'000);
  // A replayed quote whose own "now" equals its receive time would look fresh forever if trusted.
  QuoteContext replay{350'000, 0, 0};
  EXPECT_EQ(h.oms.submit(Harness::buy("k"), replay).risk, RiskReject::kStaleQuote);
}

TEST(Oms, StaleMarkOnAHeldPositionBlocksNewExposureButNotReducing) {
  OmsConfig cfg = Harness::defaultConfig();
  cfg.markMaxAgeMs = 1000;
  Harness h(cfg);
  Harness::init(h);
  // Two carried positions, both marked at bootstrap.
  h.venue.positions = {{"00700", 200, 200, 340'000, 350'000}, {"09988", 100, 100, 79'000, 80'000}};
  ASSERT_TRUE(h.oms.bootstrap().ok());
  h.clock.advanceMs(5000);  // both marks are now 5 s old
  // A fresh quote for 00700 refreshes 00700's mark, but 09988's mark is still stale, so P&L and
  // exposure cannot be trusted: new exposure is refused...
  EXPECT_EQ(h.oms.submit(Harness::buy("more"), h.quote()).risk, RiskReject::kStaleQuote);
  // ...while reducing an existing position is still allowed (a breach must not trap us).
  EXPECT_EQ(h.oms.submit(Harness::sell("flatten", 200), h.quote()).status, SubmitStatus::kAccepted);
  // Refreshing the other symbol's mark unblocks new exposure again.
  h.oms.onMark("09988", 80'000);
  // (priced below our resting flatten sell so it is not a self-trade)
  EXPECT_EQ(h.oms.submit(Harness::buy("again", 100, 349'800), h.quote()).status,
            SubmitStatus::kAccepted);
}

TEST(Oms, TwoSellsOfTheWholeHoldingSecondOneIsAShort) {
  Harness h;
  Harness::init(h);
  h.venue.positions = {{"00700", 100, 100, 340'000, 350'000}};
  ASSERT_TRUE(h.oms.bootstrap().ok());
  ASSERT_EQ(h.oms.submit(Harness::sell("s1", 100, 351'000), h.quote()).status,
            SubmitStatus::kAccepted);
  const auto second = h.oms.submit(Harness::sell("s2", 100, 352'000), h.quote());
  EXPECT_EQ(second.status, SubmitStatus::kRejectedByRisk);
  EXPECT_EQ(second.risk, RiskReject::kShortSale);  // the first sell already commits the shares
}

TEST(Oms, SecondSellIsPlacedAsShortWhenShortingIsAllowed) {
  PreTradeConfig pt = Harness::defaultPreTrade();
  pt.allowShort = true;
  Harness h(Harness::defaultConfig(),
            {.maxPerWindow = 100, .windowMs = 30'000, .reservedForCancels = 10}, pt);
  Harness::init(h);
  h.venue.positions = {{"00700", 100, 100, 340'000, 350'000}};
  ASSERT_TRUE(h.oms.bootstrap().ok());
  ASSERT_EQ(h.oms.submit(Harness::sell("s1", 100, 351'000), h.quote()).status,
            SubmitStatus::kAccepted);
  ASSERT_EQ(h.oms.submit(Harness::sell("s2", 100, 352'000), h.quote()).status,
            SubmitStatus::kAccepted);
  EXPECT_FALSE(h.venue.placed[0].sellShort);
  EXPECT_TRUE(h.venue.placed[1].sellShort);  // the shares were already committed to the first
}

TEST(Oms, OrderThatWouldTradeAgainstOurOwnRestingOrderIsRejected) {
  Harness h;
  Harness::init(h);
  h.venue.positions = {{"00700", 100, 100, 340'000, 350'000}};
  ASSERT_TRUE(h.oms.bootstrap().ok());
  ASSERT_EQ(h.oms.submit(Harness::sell("rest", 100, 351'000), h.quote()).status,
            SubmitStatus::kAccepted);
  EXPECT_EQ(h.oms.submit(Harness::buy("cross", 100, 351'000), h.quote()).risk,
            RiskReject::kSelfTrade);  // a buy at or above our resting sell
  EXPECT_EQ(h.oms.submit(Harness::buy("below", 100, 350'000), h.quote()).status,
            SubmitStatus::kAccepted);  // strictly below does not cross
}

// --- Push routing -------------------------------------------------------------------------------

namespace {
opend::Frame orderPush(int env, std::uint64_t acc, std::uint64_t orderId,
                       const std::string& remark) {
  Trd_UpdateOrder::Response rsp;
  rsp.set_rettype(Common::RetType_Succeed);
  auto* header = rsp.mutable_s2c()->mutable_header();
  header->set_trdenv(env);
  header->set_accid(acc);
  header->set_trdmarket(1);
  auto* order = rsp.mutable_s2c()->mutable_order();
  order->set_trdside(1);
  order->set_ordertype(1);
  order->set_orderstatus(5);
  order->set_orderid(orderId);
  order->set_orderidex("x");
  order->set_code("00700");
  order->set_name("x");
  order->set_qty(100);
  order->set_price(350.0);
  order->set_createtime("t");
  order->set_updatetime("t");
  order->set_remark(remark);
  const std::string body = rsp.SerializeAsString();
  opend::Frame frame;
  frame.protoId = opend::protoId::kTrdUpdateOrder;
  frame.body.assign(body.begin(), body.end());
  return frame;
}
}  // namespace

TEST(PushRouter, RoutesOnlyEventsForOurAccountEnvironmentAndMarket) {
  Harness h;
  h.boot();
  PushRouter router(h.oms, TradeTarget::simulate(111, opend::TrdMarket::kHK));
  router.onFrame(orderPush(0, 111, 1, "FT-X-1"));  // ours
  router.onFrame(orderPush(1, 111, 2, "FT-X-2"));  // right account id, but REAL not SIMULATE
  router.onFrame(orderPush(0, 999, 3, "FT-X-3"));  // another account
  EXPECT_EQ(router.routed(), 1U);
  EXPECT_EQ(router.foreign(), 2U);
  EXPECT_EQ(h.oms.liveOrderCount(), 1U);  // only the first was adopted into our book
}

TEST(PushRouter, CountsUndecodablePushesWithoutDisturbingTheOms) {
  Harness h;
  h.boot();
  PushRouter router(h.oms, TradeTarget::simulate(111, opend::TrdMarket::kHK));
  opend::Frame garbage;
  garbage.protoId = opend::protoId::kTrdUpdateOrder;
  garbage.body = {0xFF, 0xFF, 0x01};
  router.onFrame(garbage);
  garbage.protoId = opend::protoId::kTrdUpdateOrderFill;
  router.onFrame(garbage);
  auto badMarket = orderPush(0, 111, 5, "FT-X-5");
  Trd_UpdateOrder::Response rsp;
  rsp.ParseFromArray(badMarket.body.data(), static_cast<int>(badMarket.body.size()));
  rsp.mutable_s2c()->mutable_header()->set_trdmarket(99);
  const std::string body = rsp.SerializeAsString();
  badMarket.body.assign(body.begin(), body.end());
  router.onFrame(badMarket);
  EXPECT_EQ(router.undecodable(), 3U);
  EXPECT_EQ(router.routed(), 0U);
  EXPECT_EQ(h.oms.liveOrderCount(), 0U);
}

TEST(PushRouter, IgnoresUnrelatedProtocols) {
  Harness h;
  h.boot();
  PushRouter router(h.oms, TradeTarget::simulate(111, opend::TrdMarket::kHK));
  opend::Frame quote;
  quote.protoId = opend::protoId::kQotUpdateBasicQot;
  router.onFrame(quote);
  EXPECT_EQ(router.routed() + router.foreign() + router.undecodable(), 0U);
}
