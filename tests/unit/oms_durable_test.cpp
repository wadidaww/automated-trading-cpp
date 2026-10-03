#include <gtest/gtest.h>

#include <filesystem>

#include "futu_trader/app/metrics_bindings.hpp"
#include "futu_trader/engine/engine.hpp"
#include "futu_trader/infra/wal.hpp"
#include "futu_trader/oms/journal_codec.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/strategy/strategies.hpp"

using namespace futu_trader;
using namespace futu_trader::oms;

namespace {

// A broker that can "lose the reply": it records the order but answers with a timeout.
class LossyVenue : public IVenue {
 public:
  Result<opend::PlacedOrder> place(const opend::PlaceOrderRequest& request) override {
    ++placeCalls;
    if (!reachBroker) {
      return Error{ErrorCode::kTimeout, "send never reached the broker"};
    }
    opend::BrokerOrder order;
    order.orderId = nextId++;
    order.code = request.code;
    order.side = request.side;
    order.qty = request.qty;
    order.priceMills = request.priceMills;
    order.status = 5;
    order.remark = request.remark;
    orders.push_back(order);
    if (loseReply) {
      return Error{ErrorCode::kTimeout, "reply lost"};
    }
    return opend::PlacedOrder{order.orderId, "EX"};
  }
  Result<bool> cancel(std::uint64_t) override { return true; }
  Result<std::vector<opend::BrokerOrder>> listOrders() override { return orders; }
  Result<std::vector<opend::BrokerFill>> listFills() override {
    return std::vector<opend::BrokerFill>{};
  }
  Result<std::vector<opend::PositionInfo>> listPositions() override {
    return std::vector<opend::PositionInfo>{};
  }
  Result<opend::FundsInfo> funds() override { return opend::FundsInfo{}; }

  bool reachBroker{true};
  bool loseReply{false};
  int placeCalls{0};
  std::uint64_t nextId{500};
  std::vector<opend::BrokerOrder> orders;  // the "exchange": survives our restarts
};

// One process lifetime: everything that dies in a crash.
struct Process {
  Process(LossyVenue& v, ManualClock& c, const std::string& epoch)
      : venue(v),
        clock(c),
        rate({.maxPerWindow = 100, .windowMs = 30'000, .reservedForCancels = 10}, clock),
        risk(limits(), pre(), kill, instruments),
        oms(venue, risk, rate, kill, book, clock, config(epoch)) {
    instruments.add({"00700", 100});
  }
  static OmsConfig config(const std::string& epoch) {
    OmsConfig cfg;
    cfg.sessionEpoch = epoch;
    cfg.reserveOrders = 100;
    return cfg;
  }
  static PreTradeConfig pre() {
    return {.priceBandBps = 500,
            .maxQuoteAgeMs = 2000,
            .maxOrderNotionalMills = 1'000'000'000,
            .allowShort = false};
  }
  static RiskConfig limits() {
    return {.maxPositionNotionalMinor = 2'000'000'000,
            .maxPortfolioNotionalMinor = 10'000'000'000,
            .maxDailyLossMinor = 50'000'000,
            .maxOpenOrders = 50,
            .concentrationLimit = 1.0};
  }
  QuoteContext quote() const { return {350'000, clock.nowNs(), clock.nowNs()}; }

  LossyVenue& venue;
  ManualClock& clock;
  execution::KillSwitch kill;
  instrument::InstrumentTable instruments;
  portfolio::PositionBook book;
  execution::RateLimiter rate;
  PreTradeRisk risk;
  Oms oms;
};

OrderIntent buy(const std::string& key) { return {key, "00700", Side::kBuy, 100, 350'000}; }

std::vector<DurableSubmit> loadIntents(const std::string& path) {
  std::vector<DurableSubmit> out;
  for (const auto& record : infra::readWal(path).records) {
    if (auto submit = decodeSubmit(record)) {
      out.push_back(*submit);
    }
  }
  return out;
}

struct TempWal {
  explicit TempWal(const std::string& name)
      : path((std::filesystem::temp_directory_path() / name).string()) {
    std::filesystem::remove(path);
  }
  ~TempWal() { std::filesystem::remove(path); }
  std::string path;
};

}  // namespace

TEST(JournalCodec, SubmitAndJournalRoundTrip) {
  const DurableSubmit in{123456789, "FT-E-1", "key|with|bars\n", "00700", Side::kSell,
                         300,       351'500};
  const auto out = decodeSubmit(encodeSubmit(in));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->tsNs, in.tsNs);
  EXPECT_EQ(out->clOrdId, in.clOrdId);
  EXPECT_EQ(out->intentKey, in.intentKey);
  EXPECT_EQ(out->symbol, in.symbol);
  EXPECT_EQ(out->side, Side::kSell);
  EXPECT_EQ(out->qty, 300);
  EXPECT_EQ(out->priceMills, 351'500);

  const JournalEntry entry{42, JournalKind::kHalt, "FT-E-2", std::string("a\0b", 3)};
  const auto back = decodeJournal(encodeJournal(entry));
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->kind, JournalKind::kHalt);
  EXPECT_EQ(back->detail, entry.detail);
}

TEST(JournalCodec, EveryTruncationAndWrongKindIsRejectedNotMisread) {
  const std::string bytes = encodeSubmit({1, "FT-E-1", "k", "00700", Side::kBuy, 100, 350'000});
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    EXPECT_FALSE(decodeSubmit(bytes.substr(0, length)).has_value()) << "length " << length;
  }
  EXPECT_FALSE(decodeSubmit(bytes + "x").has_value());  // trailing garbage
  EXPECT_FALSE(decodeJournal(bytes).has_value());       // a submit is not a journal entry
  std::string badSide = bytes;
  badSide[bytes.size() - 17] = 7;  // the side byte
  EXPECT_FALSE(decodeSubmit(badSide).has_value());
  EXPECT_FALSE(decodeSubmit(std::string(1000, '\xFF')).has_value());  // huge length prefixes
}

TEST(OmsDurable, IntentIsLoggedBeforeTheOrderIsSent) {
  ManualClock clock;
  LossyVenue venue;
  Process p(venue, clock, "A");
  ASSERT_TRUE(p.oms.bootstrap().ok());
  int placedWhenLogged = -1;
  std::string loggedId;
  p.oms.setDurableSubmitSink([&](const DurableSubmit& d) -> Result<bool> {
    placedWhenLogged = venue.placeCalls;  // must still be zero: the log comes first
    loggedId = d.clOrdId;
    return true;
  });
  const auto result = p.oms.submit(buy("k1"), p.quote());
  EXPECT_EQ(result.status, SubmitStatus::kAccepted);
  EXPECT_EQ(placedWhenLogged, 0);
  EXPECT_EQ(loggedId, result.clOrdId);
  EXPECT_EQ(venue.placeCalls, 1);
}

TEST(OmsDurable, SinkRunsWithNoOmsLockHeld) {
  ManualClock clock;
  LossyVenue venue;
  Process p(venue, clock, "A");
  ASSERT_TRUE(p.oms.bootstrap().ok());
  p.oms.setDurableSubmitSink([&](const DurableSubmit&) -> Result<bool> {
    // Re-entering the OMS from the sink would deadlock if the lock were held.
    EXPECT_EQ(p.oms.liveOrderCount(), 1U);
    return true;
  });
  EXPECT_EQ(p.oms.submit(buy("k1"), p.quote()).status, SubmitStatus::kAccepted);
}

TEST(OmsDurable, LogFailureSendsNothingRejectsAndHaltsFailClosed) {
  ManualClock clock;
  LossyVenue venue;
  Process p(venue, clock, "A");
  ASSERT_TRUE(p.oms.bootstrap().ok());
  p.oms.setDurableSubmitSink(
      [](const DurableSubmit&) -> Result<bool> { return Error{ErrorCode::kServer, "disk full"}; });
  const auto result = p.oms.submit(buy("k1"), p.quote());
  EXPECT_EQ(result.status, SubmitStatus::kNotDurable);
  EXPECT_EQ(venue.placeCalls, 0);  // never reached the broker
  EXPECT_TRUE(p.kill.tripped());
  EXPECT_EQ(p.oms.liveOrderCount(), 0U);
  EXPECT_EQ(p.oms.stats().durableFailures.load(), 1U);
  // Later submits fail too (kill switch), even with a healthy log.
  p.oms.setDurableSubmitSink([](const DurableSubmit&) -> Result<bool> { return true; });
  EXPECT_NE(p.oms.submit(buy("k2"), p.quote()).status, SubmitStatus::kAccepted);
  EXPECT_EQ(venue.placeCalls, 0);
}

TEST(OmsDurable, StatsCountSubmitOutcomesAndRiskReasons) {
  ManualClock clock;
  LossyVenue venue;
  Process p(venue, clock, "A");
  ASSERT_TRUE(p.oms.bootstrap().ok());
  EXPECT_EQ(p.oms.submit(buy("k1"), p.quote()).status, SubmitStatus::kAccepted);
  EXPECT_EQ(p.oms.submit(buy("k1"), p.quote()).status, SubmitStatus::kDuplicate);
  OrderIntent odd = buy("k2");
  odd.qty = 150;  // not a board-lot multiple
  EXPECT_EQ(p.oms.submit(odd, p.quote()).status, SubmitStatus::kRejectedByRisk);
  const auto& stats = p.oms.stats();
  EXPECT_EQ(stats.submits[static_cast<std::size_t>(SubmitStatus::kAccepted)].load(), 1U);
  EXPECT_EQ(stats.submits[static_cast<std::size_t>(SubmitStatus::kDuplicate)].load(), 1U);
  EXPECT_EQ(stats.riskRejects[static_cast<std::size_t>(RiskReject::kLotSize)].load(), 1U);
}

TEST(OmsDurable, OversizeIntentFieldsAreInvalidNotTruncated) {
  ManualClock clock;
  LossyVenue venue;
  Process p(venue, clock, "A");
  ASSERT_TRUE(p.oms.bootstrap().ok());
  EXPECT_EQ(p.oms.submit(buy(std::string(5000, 'k')), p.quote()).status, SubmitStatus::kInvalid);
}

// ---- Game day: kill the process mid-order, restart, no duplicate
// ---------------------------------

TEST(GameDay, CrashAfterTheBrokerAcceptedButBeforeWeSawTheReplyDoesNotDuplicate) {
  TempWal wal("futu_gameday_a.wal");
  ManualClock clock;
  LossyVenue venue;
  venue.loseReply = true;  // the order exists at the broker, the process never learns it
  {
    Process before(venue, clock, "RUN1");
    auto log = infra::Wal::open({wal.path});
    ASSERT_TRUE(log.ok());
    before.oms.setDurableSubmitSink(
        [&](const DurableSubmit& d) { return log.value()->appendDurable(encodeSubmit(d)); });
    ASSERT_TRUE(before.oms.bootstrap().ok());
    EXPECT_EQ(before.oms.submit(buy("signal-1"), before.quote()).status, SubmitStatus::kAmbiguous);
  }  // "kill -9": all in-memory state is gone; only the WAL file and the broker remain
  ASSERT_EQ(venue.orders.size(), 1U);

  clock.advanceMs(10'000);
  venue.loseReply = false;
  Process after(venue, clock, "RUN2");
  const auto intents = loadIntents(wal.path);
  ASSERT_EQ(intents.size(), 1U);
  EXPECT_EQ(after.oms.restoreIntents(intents, 0), 1U);
  ASSERT_TRUE(after.oms.bootstrap().ok());

  // The strategy, knowing nothing, repeats the same signal: it must NOT create a second order.
  const auto retry = after.oms.submit(buy("signal-1"), after.quote());
  EXPECT_EQ(retry.status, SubmitStatus::kBlockedUnresolved);
  EXPECT_EQ(venue.placeCalls, 1);

  const auto report = after.oms.reconcile();  // finds it by its ClOrdId remark
  EXPECT_TRUE(report.clean());
  EXPECT_EQ(report.resolvedUnknown, 1U);
  EXPECT_EQ(after.oms.unresolvedCount(), 0U);
  EXPECT_EQ(after.oms.submit(buy("signal-1"), after.quote()).status, SubmitStatus::kDuplicate);
  EXPECT_EQ(venue.placeCalls, 1);
  EXPECT_EQ(venue.orders.size(), 1U);
  EXPECT_EQ(after.oms.liveOrderCount(), 1U);  // adopted under its own id, not as a stranger
  EXPECT_EQ(after.oms.orders().front().external, false);
}

TEST(GameDay, CrashBeforeTheOrderLeftTheProcessStaysBlockedNotResent) {
  TempWal wal("futu_gameday_b.wal");
  ManualClock clock;
  LossyVenue venue;
  venue.reachBroker = false;
  {
    Process before(venue, clock, "RUN1");
    auto log = infra::Wal::open({wal.path});
    ASSERT_TRUE(log.ok());
    before.oms.setDurableSubmitSink(
        [&](const DurableSubmit& d) { return log.value()->appendDurable(encodeSubmit(d)); });
    ASSERT_TRUE(before.oms.bootstrap().ok());
    before.oms.submit(buy("signal-1"), before.quote());
  }
  ASSERT_TRUE(venue.orders.empty());

  clock.advanceMs(10'000);
  Process after(venue, clock, "RUN2");
  EXPECT_EQ(after.oms.restoreIntents(loadIntents(wal.path), 0), 1U);
  ASSERT_TRUE(after.oms.bootstrap().ok());
  for (int i = 0; i < 4; ++i) {  // grace + several clean listings that all lack it
    clock.advanceMs(2'000);
    EXPECT_TRUE(after.oms.reconcile().clean());
  }
  EXPECT_EQ(after.oms.unresolvedCount(), 0U);
  // Declared dead by absence: the key stays blocked (a human or a new key decides), never
  // auto-resent.
  EXPECT_EQ(after.oms.submit(buy("signal-1"), after.quote()).status, SubmitStatus::kDuplicate);
  EXPECT_EQ(venue.placeCalls, 1);
  // A new intent for the symbol is fine again.
  EXPECT_EQ(after.oms.submit(buy("signal-2"), after.quote()).status, SubmitStatus::kAmbiguous);
}

TEST(GameDay, OldDaysIntentsAreNotRestored) {
  ManualClock clock;
  LossyVenue venue;
  Process p(venue, clock, "RUN2");
  const std::vector<DurableSubmit> old = {
      {100, "FT-OLD-1", "yesterday", "00700", Side::kBuy, 100, 350'000},
      {900, "FT-OLD-2", "today", "00700", Side::kBuy, 100, 350'000},
      {900, "not-ours-3", "foreign", "00700", Side::kBuy, 100, 350'000},
      {900, "FT-OLD-2", "dup", "00700", Side::kBuy, 100, 350'000}};
  EXPECT_EQ(p.oms.restoreIntents(old, 500), 1U);  // only "today"; foreign ids and dups ignored
  EXPECT_EQ(p.oms.stats().restoredIntents.load(), 1U);
}

TEST(OmsShutdown, CancelAllLiveCancelsRestingOrdersWithoutTrippingTheKillSwitch) {
  ManualClock clock;
  LossyVenue venue;
  Process p(venue, clock, "A");
  ASSERT_TRUE(p.oms.bootstrap().ok());
  ASSERT_EQ(p.oms.submit(buy("k1"), p.quote()).status, SubmitStatus::kAccepted);
  const auto report = p.oms.cancelAllLive();
  EXPECT_EQ(report.cancelRequested, 1U);
  EXPECT_EQ(report.cancelFailed, 0U);
  EXPECT_FALSE(p.kill.tripped());
  EXPECT_FALSE(p.oms.haltPending());
}

// ---- Metrics bindings
// ------------------------------------------------------------------------------

namespace {
struct NoopStrategy final : strategy::IStrategy {
  void onQuote(const QuoteEvent&, strategy::StrategyContext&) override {}
};
}  // namespace

TEST(MetricsBindings, OmsMetricsRenderLiveValuesWithTheDocumentedNames) {
  ManualClock clock;
  LossyVenue venue;
  Process p(venue, clock, "A");
  ASSERT_TRUE(p.oms.bootstrap().ok());
  infra::MetricsRegistry registry;
  ASSERT_TRUE(app::registerOmsMetrics(registry, p.oms, p.kill, p.rate).ok());
  const auto before = registry.render();
  EXPECT_NE(before.find("futu_oms_submits_total{status=\"accepted\"} 0"), std::string::npos);
  EXPECT_NE(before.find("futu_kill_switch_tripped 0"), std::string::npos);

  ASSERT_EQ(p.oms.submit(buy("k1"), p.quote()).status, SubmitStatus::kAccepted);
  OrderIntent odd = buy("k2");
  odd.qty = 150;
  p.oms.submit(odd, p.quote());
  p.kill.trip("test");
  const auto after = registry.render();
  EXPECT_NE(after.find("futu_oms_submits_total{status=\"accepted\"} 1"), std::string::npos);
  EXPECT_NE(after.find("futu_oms_risk_rejects_total{reason=\"lot_size\"} 1"), std::string::npos)
      << after;
  EXPECT_NE(after.find("futu_oms_live_orders 1"), std::string::npos);
  EXPECT_NE(after.find("futu_kill_switch_tripped 1"), std::string::npos);
  EXPECT_NE(after.find("futu_rate_limit_utilization"), std::string::npos);
}

TEST(MetricsBindings, RegisteringTwiceIsRefusedNotSilentlyDuplicated) {
  ManualClock clock;
  LossyVenue venue;
  Process p(venue, clock, "A");
  infra::MetricsRegistry registry;
  ASSERT_TRUE(app::registerOmsMetrics(registry, p.oms, p.kill, p.rate).ok());
  EXPECT_FALSE(app::registerOmsMetrics(registry, p.oms, p.kill, p.rate).ok());
}

TEST(MetricsBindings, EngineAndWalMetricsRender) {
  ManualClock clock;
  LossyVenue venue;
  Process p(venue, clock, "A");
  NoopStrategy strat;
  engine::EngineConfig cfg;
  cfg.ringCapacity = 64;
  engine::Engine eng(p.oms, strat, p.book, clock, p.kill, cfg);
  TempWal wal("futu_metrics_bind.wal");
  auto log = infra::Wal::open({wal.path});
  ASSERT_TRUE(log.ok());
  ASSERT_TRUE(log.value()->appendDurable("x").ok());

  infra::MetricsRegistry registry;
  ASSERT_TRUE(app::registerEngineMetrics(registry, eng).ok());
  ASSERT_TRUE(app::registerWalMetrics(registry, *log.value()).ok());
  const auto text = registry.render();
  EXPECT_NE(text.find("futu_engine_quotes_dropped_total 0"), std::string::npos);
  EXPECT_NE(text.find("futu_engine_handle_seconds_bucket"), std::string::npos);
  EXPECT_NE(text.find("futu_wal_records_total 1"), std::string::npos);
  EXPECT_NE(text.find("futu_wal_failed 0"), std::string::npos);
  EXPECT_NE(text.find("futu_wal_sync_seconds_count"), std::string::npos);
}
