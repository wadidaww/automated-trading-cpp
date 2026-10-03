#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <thread>

#include "futu_trader/oms/live_gate.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/oms/opend_venue.hpp"
#include "futu_trader/oms/push_router.hpp"
#include "futu_trader/opend/proto_ids.hpp"
#include "futu_trader/opend/trade_decode.hpp"
#include "mock_opend.hpp"
#include "test_support.hpp"

using namespace futu_trader;
using testing_support::waitFor;
using namespace futu_trader::oms;
using namespace std::chrono_literals;

namespace {

// The whole stack: mock OpenD <-> OpenDClient <-> OpenDVenue <-> Oms, with pushes wired in.
struct Stack {
  explicit Stack(const TradeTarget& target)
      : port(server.start()),
        client(makeConfig(port)),
        venue(client, target),
        target_(target),
        rate({.maxPerWindow = 100, .windowMs = 30'000, .reservedForCancels = 10}, clock),
        risk({.maxPositionNotionalMinor = 2'000'000'000,
              .maxPortfolioNotionalMinor = 10'000'000'000,
              .maxDailyLossMinor = 50'000'000,
              .maxOpenOrders = 50,
              .concentrationLimit = 1.0},
             {.priceBandBps = 500,
              .maxQuoteAgeMs = 2000,
              .maxOrderNotionalMills = 1'000'000'000,
              .allowShort = false},
             kill, instruments),
        oms(venue, risk, rate, kill, book, clock, config()),
        router(oms, target) {
    instruments.add({"00700", 100});
    client.setPushHandler([this](const opend::Frame& frame) { router.onFrame(frame); });
  }
  ~Stack() {
    client.close();
    server.stop();
  }

  static opend::ClientConfig makeConfig(std::uint16_t port) {
    opend::ClientConfig cfg;
    cfg.connection.port = port;
    cfg.connection.requestTimeout = 500ms;
    cfg.reconnectBase = 20ms;
    cfg.reconnectMax = 200ms;
    return cfg;
  }
  static OmsConfig config() {
    OmsConfig cfg;
    cfg.sessionEpoch = "E2E";
    cfg.ambiguityGraceMs = 0;  // ManualClock does not advance; resolve immediately in tests
    return cfg;
  }
  QuoteContext quote(Money last = 350'000) const { return {last, clock.nowNs(), clock.nowNs()}; }
  static OrderIntent buy(const std::string& key, std::int64_t qty = 100) {
    return {key, "00700", Side::kBuy, qty, 350'000};
  }
  void start() {
    ASSERT_TRUE(client.connect().ok());
    ASSERT_TRUE(client.subscribeAccountPush({target_.accId()}).ok());
    ASSERT_TRUE(oms.bootstrap().ok());
  }

  mock::MockOpenD server;
  std::uint16_t port;
  opend::OpenDClient client;
  OpenDVenue venue;
  TradeTarget target_;
  ManualClock clock;
  execution::KillSwitch kill;
  instrument::InstrumentTable instruments;
  portfolio::PositionBook book;
  execution::RateLimiter rate;
  PreTradeRisk risk;
  Oms oms;
  PushRouter router;
};

TradeTarget simTarget() { return TradeTarget::simulate(111, opend::TrdMarket::kHK); }

}  // namespace

TEST(OmsIntegration, OrderFlowsToBrokerFillsPushBackAndPositionsUpdate) {
  Stack s(simTarget());
  s.start();
  const auto result = s.oms.submit(Stack::buy("k1"), s.quote());
  ASSERT_EQ(result.status, SubmitStatus::kAccepted) << result.detail;

  ASSERT_EQ(s.server.orders().size(), 1U);
  EXPECT_EQ(s.server.orders()[0].remark, result.clOrdId);  // the ClOrdId is what the broker holds
  EXPECT_EQ(s.server.orders()[0].trdEnv, 0);               // SIMULATE, stamped by the venue
  EXPECT_EQ(s.server.orders()[0].accId, 111U);
  EXPECT_DOUBLE_EQ(s.server.orders()[0].price, 350.0);

  ASSERT_TRUE(s.server.fillOrderByRemark(result.clOrdId, 100, 350.0));
  ASSERT_TRUE(waitFor([&] { return s.oms.order(result.clOrdId)->state == OmsState::kFilled; }));
  ASSERT_TRUE(waitFor([&] { return s.book.qty("00700") == 100; }));
  EXPECT_EQ(s.book.totalFees() > 0, true);

  s.server.setPositions({{"00700", 100, 100, 350.0, 350.0}});
  const auto report = s.oms.reconcile();
  EXPECT_TRUE(report.clean()) << (report.drifts.empty() ? report.error : report.drifts[0].detail);
  EXPECT_FALSE(s.kill.tripped());
}

TEST(OmsIntegration, LostResponseButPushArrivesResolvesTheAmbiguityByItself) {
  // The realistic happy-ish case: the reply is lost, but the order-status push still arrives.
  Stack s(simTarget());
  s.start();
  mock::Faults faults;
  faults.dropResponses = 1;
  s.server.setFaults(faults);
  const auto first = s.oms.submit(Stack::buy("k"), s.quote());
  ASSERT_EQ(first.status, SubmitStatus::kAmbiguous);
  ASSERT_TRUE(waitFor([&] { return s.oms.order(first.clOrdId)->state == OmsState::kWorking; }));
  EXPECT_EQ(s.oms.order(first.clOrdId)->venueOrderId, 5000U);  // learned from the push, by remark
  EXPECT_EQ(s.oms.submit(Stack::buy("k"), s.quote()).status, SubmitStatus::kDuplicate);
  EXPECT_EQ(s.server.orders().size(), 1U);
}

TEST(OmsIntegration, LostResponseIsHealedByReconcileWithoutADuplicateOrder) {
  Stack s(simTarget());
  s.start();
  s.server.setSuppressPushes(true);  // worst case: no push either
  mock::Faults faults;
  faults.dropResponses = 1;  // the broker takes the order, the reply is lost
  s.server.setFaults(faults);

  const auto first = s.oms.submit(Stack::buy("k"), s.quote());
  ASSERT_EQ(first.status, SubmitStatus::kAmbiguous);
  ASSERT_EQ(s.server.orders().size(), 1U);

  // Retrying the same intent before reconciling is blocked...
  EXPECT_EQ(s.oms.submit(Stack::buy("k"), s.quote()).status, SubmitStatus::kBlockedUnresolved);
  EXPECT_EQ(s.server.placeRequests(), 1U);

  // ...and reconciling discovers the order by its remark.
  const auto report = s.oms.reconcile();
  ASSERT_TRUE(report.complete);
  EXPECT_EQ(report.resolvedUnknown, 1U);
  EXPECT_EQ(s.oms.order(first.clOrdId)->state, OmsState::kWorking);
  EXPECT_EQ(s.oms.submit(Stack::buy("k"), s.quote()).status, SubmitStatus::kDuplicate);
  EXPECT_EQ(s.server.orders().size(), 1U);  // exactly one order, ever
}

TEST(OmsIntegration, ConnectionDropMidResponseIsAlsoAmbiguousThenResolved) {
  Stack s(simTarget());
  s.start();
  mock::Faults faults;
  faults.disconnectMidFrame = 1;
  s.server.setFaults(faults);
  const auto result = s.oms.submit(Stack::buy("k"), s.quote());
  ASSERT_EQ(result.status, SubmitStatus::kAmbiguous);
  ASSERT_TRUE(waitFor([&] { return s.client.reconnectCount() >= 1 && s.client.isConnected(); }));

  const auto report = s.oms.reconcile();
  ASSERT_TRUE(report.complete);
  EXPECT_EQ(s.oms.order(result.clOrdId)->state, OmsState::kWorking);
  EXPECT_EQ(s.server.orders().size(), 1U);
}

TEST(OmsIntegration, MissedPushesAreRecoveredFromTheOrderAndFillLists) {
  Stack s(simTarget());
  s.start();
  const auto result = s.oms.submit(Stack::buy("k"), s.quote());
  ASSERT_EQ(result.status, SubmitStatus::kAccepted);

  s.server.setSuppressPushes(true);  // the fill happens but we are never told
  ASSERT_TRUE(s.server.fillOrderByRemark(result.clOrdId, 100, 350.0));
  s.server.setPositions({{"00700", 100, 100, 350.0, 350.0}});
  EXPECT_EQ(s.book.qty("00700"), 0);  // we are blind

  const auto report = s.oms.reconcile();
  ASSERT_TRUE(report.complete);
  EXPECT_EQ(report.missedFillsApplied, 1U);
  EXPECT_EQ(report.statusCatchUps, 1U);
  EXPECT_TRUE(report.drifts.empty());
  EXPECT_EQ(s.book.qty("00700"), 100);
  EXPECT_EQ(s.oms.order(result.clOrdId)->state, OmsState::kFilled);
}

TEST(OmsIntegration, BrokerRejectionIsDefiniteAndRetryable) {
  Stack s(simTarget());
  s.start();
  s.server.rejectNextPlace("insufficient buying power");
  const auto first = s.oms.submit(Stack::buy("k"), s.quote());
  EXPECT_EQ(first.status, SubmitStatus::kRejectedByVenue);
  EXPECT_NE(first.detail.find("insufficient buying power"), std::string::npos);
  EXPECT_EQ(s.oms.submit(Stack::buy("k"), s.quote()).status, SubmitStatus::kAccepted);
}

TEST(OmsIntegration, HaltCancelsWorkingOrdersAtTheBroker) {
  Stack s(simTarget());
  s.start();
  ASSERT_EQ(s.oms.submit(Stack::buy("a"), s.quote()).status, SubmitStatus::kAccepted);
  ASSERT_EQ(s.oms.submit(Stack::buy("b"), s.quote()).status, SubmitStatus::kAccepted);
  const auto report = s.oms.haltAndCancelAll("drill");
  EXPECT_EQ(report.cancelRequested, 2U);
  EXPECT_EQ(report.cancelFailed, 0U);
  for (const auto& order : s.server.orders()) {
    EXPECT_EQ(order.status, 15);  // Cancelled_All at the broker
  }
  ASSERT_TRUE(waitFor([&] { return s.oms.liveOrderCount() == 0U; }));
  EXPECT_EQ(s.oms.submit(Stack::buy("c"), s.quote()).risk, RiskReject::kKillSwitch);
}

TEST(OmsIntegration, ReconcileDetectsAPhantomPositionAndHalts) {
  Stack s(simTarget());
  s.start();
  s.server.setPositions({{"00700", 500, 500, 350.0, 350.0}});  // someone else bought
  const auto report = s.oms.reconcile();
  ASSERT_EQ(report.drifts.size(), 1U);
  EXPECT_TRUE(s.kill.tripped());
}

TEST(OmsIntegration, BootstrapAdoptsPreexistingBrokerState) {
  mock::MockOpenD probe;  // just to show the shape: state exists before the OMS starts
  Stack s(simTarget());
  s.server.setPositions({{"00700", 200, 200, 340.0, 340.0}});
  mock::MockOrder resting;
  resting.remark = "FT-PREVIOUS-1";
  resting.code = "00700";
  resting.qty = 100;
  resting.price = 351.0;
  resting.status = 5;
  resting.accId = 111;
  resting.trdEnv = 0;
  s.server.injectOrder(resting);
  s.start();
  EXPECT_EQ(s.book.qty("00700"), 200);
  const auto adopted = s.oms.order("FT-PREVIOUS-1");
  ASSERT_TRUE(adopted.has_value());
  EXPECT_FALSE(adopted->external);
  EXPECT_TRUE(s.oms.reconcile().clean());
}

TEST(OmsIntegration, RealTargetOnlyExistsThroughTheGateAndStampsRealOnTheWire) {
  const auto input = testing_support::validLiveInput(222);  // 222 is the mock's REAL account
  const auto approval = LiveGate::approveReal(input);
  ASSERT_TRUE(approval.ok()) << approval.error().message;
  const auto target = TradeTarget::real(approval.value(), 222, opend::TrdMarket::kHK);
  ASSERT_TRUE(target.ok());

  Stack s(target.value());
  s.start();
  ASSERT_EQ(s.oms.submit(Stack::buy("k"), s.quote()).status, SubmitStatus::kAccepted);
  ASSERT_EQ(s.server.orders().size(), 1U);
  EXPECT_EQ(s.server.orders()[0].trdEnv, 1);  // REAL, and only because the gate approved it
  EXPECT_EQ(s.server.orders()[0].accId, 222U);

  // Any missing condition means no approval, hence no REAL target can be constructed.
  auto blocked = input;
  blocked.cliLiveFlag = false;
  EXPECT_FALSE(LiveGate::approveReal(blocked).ok());
}

TEST(OmsIntegration, PushesForAnotherAccountOrEnvironmentAreDroppedNotApplied) {
  Stack s(simTarget());  // trading SIMULATE account 111
  s.start();
  // The broker also serves REAL account 222; a fill there must never reach our SIMULATE book.
  mock::MockOrder other;
  other.remark = "FT-OTHER-1";
  other.code = "00700";
  other.qty = 100;
  other.price = 350.0;
  other.status = 5;
  other.accId = 222;
  other.trdEnv = 1;
  s.server.injectOrder(other);
  s.client.subscribeAccountPush({111, 222});
  ASSERT_TRUE(s.server.fillOrderByRemark("FT-OTHER-1", 100, 350.0));  // pushes with acc 222/REAL
  ASSERT_TRUE(waitFor([&] { return s.router.foreign() >= 2U; }));     // order + fill updates
  EXPECT_EQ(s.book.qty("00700"), 0);
  EXPECT_EQ(s.oms.liveOrderCount(), 0U);
  EXPECT_EQ(s.router.routed(), 0U);
}
