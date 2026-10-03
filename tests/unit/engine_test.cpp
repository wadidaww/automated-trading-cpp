#include "futu_trader/engine/engine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>

#include "test_support.hpp"

using namespace futu_trader;
using testing_support::waitFor;
using namespace futu_trader::engine;
using namespace std::chrono_literals;

namespace {

// Thread-safe stub broker (the reconciler thread and the engine thread both talk to it).
class StubVenue final : public oms::IVenue {
 public:
  Result<opend::PlacedOrder> place(const opend::PlaceOrderRequest& request) override {
    std::scoped_lock lock(mu);
    opend::BrokerOrder order;
    order.orderId = nextId++;
    order.code = request.code;
    order.side = request.side;
    order.qty = request.qty;
    order.priceMills = request.priceMills;
    order.status = 5;
    order.remark = request.remark;
    orders.push_back(order);
    return opend::PlacedOrder{order.orderId, "x"};
  }
  Result<bool> cancel(std::uint64_t id) override {
    std::scoped_lock lock(mu);
    cancelled.push_back(id);
    for (auto& o : orders) {
      if (o.orderId == id) {
        o.status = 15;
      }
    }
    return true;
  }
  Result<std::vector<opend::BrokerOrder>> listOrders() override {
    std::scoped_lock lock(mu);
    return orders;
  }
  Result<std::vector<opend::BrokerFill>> listFills() override {
    std::scoped_lock lock(mu);
    return std::vector<opend::BrokerFill>{};
  }
  Result<std::vector<opend::PositionInfo>> listPositions() override {
    std::scoped_lock lock(mu);
    return positions;
  }
  Result<opend::FundsInfo> funds() override { return opend::FundsInfo{}; }

  std::size_t cancelCount() {
    std::scoped_lock lock(mu);
    return cancelled.size();
  }
  std::mutex mu;
  std::vector<opend::BrokerOrder> orders;
  std::vector<opend::PositionInfo> positions;
  std::vector<std::uint64_t> cancelled;
  std::uint64_t nextId{100};
};

template <typename ClockT>
struct Rig {
  explicit Rig(EngineConfig cfg = {})
      : rate({.maxPerWindow = 100000, .windowMs = 30'000, .reservedForCancels = 1000}, clock),
        risk({.maxPositionNotionalMinor = INT64_MAX / 4,
              .maxPortfolioNotionalMinor = INT64_MAX / 4,
              .maxDailyLossMinor = INT64_MAX / 4,
              .maxOpenOrders = 100000,
              .concentrationLimit = 1.0},
             {.priceBandBps = 5000,
              .maxQuoteAgeMs = 3'600'000,
              .maxOrderNotionalMills = INT64_MAX / 4,
              .allowShort = false},
             kill, instruments),
        oms(venue, risk, rate, kill, book, clock, omsConfig()),
        engineConfig(std::move(cfg)) {
    instruments.add({"00700", 100});
    EXPECT_TRUE(oms.bootstrap().ok());
  }
  static oms::OmsConfig omsConfig() {
    oms::OmsConfig c;
    c.sessionEpoch = "EN";
    return c;
  }
  ClockT clock;
  execution::KillSwitch kill;
  instrument::InstrumentTable instruments;
  portfolio::PositionBook book;
  StubVenue venue;
  execution::RateLimiter rate;
  oms::PreTradeRisk risk;
  oms::Oms oms;
  EngineConfig engineConfig;
};

QuoteEvent quote(std::int64_t ts, const std::string& sym = "00700") {
  return {ts, sym, 350'000, 350'200, 350'000, 1000, 1000};
}

struct Recorder final : strategy::IStrategy {
  void onQuote(const QuoteEvent& q, strategy::StrategyContext&) override {
    std::scoped_lock lock(mu);
    seen.push_back(q.tsNs);
  }
  std::size_t count() {
    std::scoped_lock lock(mu);
    return seen.size();
  }
  std::mutex mu;
  std::vector<std::int64_t> seen;
};

}  // namespace

TEST(EngineTick, RoundTripsAndRefusesBadSymbols) {
  QuoteTick tick;
  const QuoteEvent original{12345, "00700", 350'000, 350'200, 350'000, 111, 222};
  ASSERT_TRUE(toTick(original, 99, tick));
  EXPECT_EQ(tick.recvNs, 99);
  EXPECT_EQ(toEvent(tick), original);
  EXPECT_TRUE(toTick(QuoteEvent{1, std::string(7, 'A'), 1, 2, 1, 0, 0}, 0, tick));  // longest fit
  EXPECT_FALSE(
      toTick(QuoteEvent{1, std::string(8, 'A'), 1, 2, 1, 0, 0}, 0, tick));  // not truncated
  EXPECT_FALSE(toTick(QuoteEvent{1, "", 1, 2, 1, 0, 0}, 0, tick));
}

TEST(EngineTick, IsExactlyOneCacheLineSoRingSlotsNeverShareOne) {
  EXPECT_EQ(sizeof(QuoteTick), 64U);
  EXPECT_EQ(alignof(QuoteTick), 64U);
  EXPECT_TRUE(std::is_trivially_copyable_v<QuoteTick>);
  SpscRing<QuoteTick> ring(4);
  QuoteTick in;
  ASSERT_TRUE(toTick(QuoteEvent{1, "00700", 1, 2, 1, 3, 4}, 5, in));
  ASSERT_TRUE(ring.tryPush(in));
  QuoteTick out;
  ASSERT_TRUE(ring.tryPop(out));
  EXPECT_EQ(out.bidSize, 3);
}

TEST(EngineStats, ProducerEngineAndReconcilerCountersLiveOnDifferentCacheLines) {
  // Counters written by different threads must not share a line (false sharing on every quote).
  const auto line = [](const auto& stats, const auto& member) {
    return (reinterpret_cast<std::uintptr_t>(&member) - reinterpret_cast<std::uintptr_t>(&stats)) /
           64;
  };
  EngineStats stats;
  EXPECT_NE(line(stats, stats.received), line(stats, stats.processed));
  EXPECT_NE(line(stats, stats.processed), line(stats, stats.reconciles));
  EXPECT_NE(line(stats, stats.received), line(stats, stats.reconciles));
  EXPECT_EQ(line(stats, stats.dropped), line(stats, stats.received));        // same writer
  EXPECT_EQ(line(stats, stats.staleSkipped), line(stats, stats.processed));  // same writer
}

TEST(Engine, DeliversQuotesInOrderAndMeasuresQueueTime) {
  Rig<ManualClock> rig;
  Recorder strat;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  for (int i = 1; i <= 5; ++i) {
    ASSERT_TRUE(engine.onQuote(quote(i)));
  }
  rig.clock.advanceMs(3);  // 3 ms pass while the quotes wait in the queue
  EXPECT_EQ(engine.poll(), 5U);
  EXPECT_EQ(strat.seen, (std::vector<std::int64_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(engine.stats().processed.load(), 5U);
  EXPECT_EQ(engine.stats().queueNs.count(), 5U);
  EXPECT_GE(engine.stats().queueNs.percentile(0.5), 3'000'000U);  // they really did wait 3 ms
}

TEST(Engine, FullRingDropsTheNewestQuotesAndAccountsForEveryOne) {
  EngineConfig cfg;
  cfg.ringCapacity = 8;
  Rig<ManualClock> rig(cfg);
  Recorder strat;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  std::size_t accepted = 0;
  for (int i = 1; i <= 20; ++i) {
    accepted += engine.onQuote(quote(i)) ? 1U : 0U;
  }
  EXPECT_EQ(accepted, 8U);
  EXPECT_EQ(engine.stats().received.load(), 20U);
  EXPECT_EQ(engine.stats().dropped.load(), 12U);
  engine.poll();
  EXPECT_EQ(engine.stats().ringHighWater.load(), 8U);  // sampled by the consumer when it polls
  EXPECT_EQ(strat.seen, (std::vector<std::int64_t>{1, 2, 3, 4, 5, 6, 7, 8}));  // oldest survive
  EXPECT_EQ(engine.stats().processed.load() + engine.stats().dropped.load(),
            engine.stats().received.load());
}

TEST(Engine, QuotesThatWaitedTooLongAreSkippedSoTheStrategyNeverActsOnAStaleBacklog) {
  EngineConfig cfg;
  cfg.maxQuoteAgeNs = 1'000'000'000;  // 1 s
  Rig<ManualClock> rig(cfg);
  Recorder strat;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  for (int i = 1; i <= 3; ++i) {
    ASSERT_TRUE(engine.onQuote(quote(i)));
  }
  rig.clock.advanceMs(2000);              // a 2 s stall: those three quotes are now ancient history
  ASSERT_TRUE(engine.onQuote(quote(4)));  // this one is fresh
  EXPECT_EQ(engine.poll(), 4U);
  EXPECT_EQ(strat.seen, (std::vector<std::int64_t>{4}));  // only the fresh quote reached it
  EXPECT_EQ(engine.stats().staleSkipped.load(), 3U);
  EXPECT_EQ(engine.stats().processed.load(), 1U);
  EXPECT_EQ(engine.stats().queueNs.count(), 4U);  // the waits are still measured
}

TEST(Engine, StalenessCheckCanBeDisabled) {
  EngineConfig cfg;
  cfg.maxQuoteAgeNs = 0;
  Rig<ManualClock> rig(cfg);
  Recorder strat;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  engine.onQuote(quote(1));
  rig.clock.advanceMs(60'000);
  engine.poll();
  EXPECT_EQ(strat.seen.size(), 1U);
  EXPECT_EQ(engine.stats().staleSkipped.load(), 0U);
}

TEST(Engine, AnExplicitReceiveTimeLetsALoadGeneratorCountItsOwnStalls) {
  // Coordinated omission: if the sender stalls, the quote's wait must be measured from when it
  // SHOULD have arrived, not from when the stalled sender finally got around to it.
  Rig<ManualClock> rig;
  Recorder strat;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  rig.clock.advanceMs(10);
  engine.onQuoteAt(quote(1), /*scheduled for*/ 0);  // meant to arrive at t=0, sent at t=10ms
  engine.poll();
  EXPECT_GE(engine.stats().queueNs.percentile(0.5), 10'000'000U);
}

TEST(Engine, RefusesAndCountsBadSymbolsWithoutDisturbingTheRest) {
  Rig<ManualClock> rig;
  Recorder strat;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  EXPECT_FALSE(engine.onQuote(quote(1, "THIS-SYMBOL-IS-FAR-TOO-LONG")));
  EXPECT_TRUE(engine.onQuote(quote(2)));
  engine.poll();
  EXPECT_EQ(engine.stats().badSymbol.load(), 1U);
  EXPECT_EQ(strat.seen, (std::vector<std::int64_t>{2}));
}

TEST(Engine, ThreadedProducerAndEngineThreadNeverLoseReorOrDuplicateAcceptedQuotes) {
  // TSan covers the data-race side; this checks the accounting and ordering.
  EngineConfig cfg;
  cfg.ringCapacity = 1024;
  cfg.idleSleep = 50us;
  Rig<SteadyClock> rig(cfg);
  Recorder strat;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  engine.start();
  constexpr int kQuotes = 100'000;
  std::size_t accepted = 0;
  for (int i = 1; i <= kQuotes; ++i) {
    accepted += engine.onQuote(quote(i)) ? 1U : 0U;
    if (i % 512 == 0) {
      std::this_thread::yield();
    }
  }
  engine.stop();  // drains what was accepted
  EXPECT_EQ(strat.count(), accepted);
  EXPECT_EQ(engine.stats().processed.load(), accepted);
  EXPECT_EQ(engine.stats().processed.load() + engine.stats().dropped.load(),
            engine.stats().received.load());
  EXPECT_EQ(engine.stats().received.load(), static_cast<std::uint64_t>(kQuotes));
  for (std::size_t i = 1; i < strat.seen.size(); ++i) {
    ASSERT_GT(strat.seen[i], strat.seen[i - 1]) << "reordered or duplicated at " << i;
  }
}

TEST(Engine, AfterStopNewQuotesAreRefusedAndCountedAndStartReopensTheDoor) {
  Rig<SteadyClock> rig;
  Recorder strat;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  engine.start();
  ASSERT_TRUE(engine.onQuote(quote(1)));
  engine.stop();
  EXPECT_FALSE(engine.onQuote(quote(2)));  // would otherwise sit in the ring, unhandled
  EXPECT_EQ(engine.stats().rejectedAfterStop.load(), 1U);
  engine.start();  // restart is allowed
  EXPECT_TRUE(engine.onQuote(quote(3)));
  engine.stop();
  EXPECT_EQ(strat.seen, (std::vector<std::int64_t>{1, 3}));
  const auto& st = engine.stats();
  EXPECT_EQ(st.received.load(), st.processed.load() + st.staleSkipped.load() + st.dropped.load() +
                                    st.badSymbol.load() + st.rejectedAfterStop.load());
}

TEST(Engine, ConcurrentStartAndStopFromManyThreadsNeverCrashes) {
  Rig<SteadyClock> rig;
  Recorder strat;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  std::vector<std::thread> controllers;
  for (int t = 0; t < 4; ++t) {
    controllers.emplace_back([&engine, t] {
      for (int i = 0; i < 100; ++i) {
        if ((i + t) % 2 == 0) {
          engine.start();
        } else {
          engine.stop();
        }
      }
    });
  }
  for (auto& c : controllers) {
    c.join();
  }
  engine.stop();
  SUCCEED();  // before serialisation, an overlapping start()/stop() terminated the process
}

TEST(Engine, StopDrainsEveryQuoteAlreadyAccepted) {
  EngineConfig cfg;
  cfg.idleSleep = 5ms;  // a slow-waking engine thread: stop() must not strand queued quotes
  Rig<SteadyClock> rig(cfg);
  Recorder strat;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  engine.start();
  for (int i = 1; i <= 100; ++i) {
    ASSERT_TRUE(engine.onQuote(quote(i)));
  }
  engine.stop();
  EXPECT_EQ(strat.count(), 100U);
}

// --- Safety behaviour ---------------------------------------------------------------------------

namespace {
struct OrderThenAct final : strategy::IStrategy {
  enum class Then { kNothing, kThrow };
  explicit OrderThenAct(Then t) : then(t) {}
  void onQuote(const QuoteEvent& q, strategy::StrategyContext& ctx) override {
    ++calls;
    if (calls == 1) {
      ctx.submit("00700", Side::kBuy, 100, q.bid);  // a resting passive order
    } else if (calls == 2 && then == Then::kThrow) {
      throw std::runtime_error("model blew up");
    }
  }
  Then then;
  int calls{0};
};
}  // namespace

TEST(Engine, AStrategyThatThrowsIsDisabledAndItsRestingOrdersAreCancelled) {
  Rig<ManualClock> rig;
  OrderThenAct strat(OrderThenAct::Then::kThrow);
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  for (int i = 1; i <= 4; ++i) {
    engine.onQuote(quote(i));
  }
  engine.poll();
  EXPECT_TRUE(engine.strategyDisabled());
  EXPECT_EQ(engine.stats().strategyFaults.load(), 1U);
  EXPECT_TRUE(rig.kill.tripped());
  EXPECT_NE(rig.kill.reason().find("model blew up"), std::string::npos);
  EXPECT_EQ(rig.venue.cancelCount(), 1U);  // the resting order was pulled, not left unattended
  EXPECT_EQ(strat.calls, 2);               // never called again after the fault
  EXPECT_EQ(engine.stats().processed.load(), 4U);  // the engine itself kept running
}

TEST(Engine, ANonStandardExceptionFromTheStrategyIsAlsoContained) {
  struct ThrowsInt final : strategy::IStrategy {
    void onQuote(const QuoteEvent&, strategy::StrategyContext&) override { throw 42; }
  } strat;
  Rig<ManualClock> rig;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  engine.onQuote(quote(1));
  engine.onQuote(quote(2));
  engine.poll();
  EXPECT_TRUE(engine.strategyDisabled());
  EXPECT_TRUE(rig.kill.tripped());
  EXPECT_EQ(engine.stats().strategyFaults.load(), 1U);  // disabled after the first, not retried
  EXPECT_EQ(engine.stats().processed.load(), 2U);
}

#if !defined(NDEBUG) && !defined(__SANITIZE_THREAD__) && !defined(__SANITIZE_ADDRESS__)
TEST(EngineDeathTest, ReentrantConsumerIsCaughtInDebugBuilds) {
  // A strategy that calls poll() from inside its own callback breaks the single-consumer rule.
  struct Reentrant final : strategy::IStrategy {
    void onQuote(const QuoteEvent&, strategy::StrategyContext&) override { engine->poll(); }
    Engine* engine{nullptr};
  } strat;
  Rig<ManualClock> rig;
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  strat.engine = &engine;
  engine.onQuote(quote(1));
  EXPECT_DEATH(engine.poll(), "SPSC contract violated");
}
#endif

TEST(Engine, KillFlagFileHaltsAndCancelsOnceThenRearmsAfterAHumanReset) {
  const auto flag = std::filesystem::temp_directory_path() / "futu_engine_kill_flag";
  std::filesystem::remove(flag);
  EngineConfig cfg;
  cfg.killFlagPath = flag.string();
  Rig<ManualClock> rig(cfg);
  OrderThenAct strat(OrderThenAct::Then::kNothing);
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  engine.onQuote(quote(1));
  engine.poll();
  ASSERT_EQ(rig.oms.liveOrderCount(), 1U);

  engine.housekeeping();  // no flag yet
  EXPECT_FALSE(rig.kill.tripped());
  std::ofstream(flag).put('x');
  engine.housekeeping();
  EXPECT_TRUE(rig.kill.tripped());
  EXPECT_EQ(rig.venue.cancelCount(), 1U);
  engine.housekeeping();
  engine.housekeeping();
  EXPECT_EQ(rig.venue.cancelCount(), 1U);  // cancelled once, not on every beat

  std::filesystem::remove(flag);
  ASSERT_TRUE(rig.kill.reset("alice"));
  engine.housekeeping();  // re-arms
  std::ofstream(flag).put('x');
  engine.housekeeping();
  EXPECT_TRUE(rig.kill.tripped());  // a second incident is handled too
  std::filesystem::remove(flag);
}

TEST(Engine, HousekeepingCompletesAHaltRequestedFromThePushThread) {
  Rig<ManualClock> rig;
  OrderThenAct strat(OrderThenAct::Then::kNothing);
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  engine.onQuote(quote(1));
  engine.poll();
  ASSERT_EQ(rig.oms.liveOrderCount(), 1U);
  opend::BrokerFill busted;  // arrives on the push thread, which must not call the broker itself
  busted.fillId = "F1";
  busted.code = "00700";
  busted.qty = 100;
  busted.priceMills = 350'000;
  busted.status = 1;
  rig.oms.onFill(busted);
  EXPECT_EQ(rig.venue.cancelCount(), 0U);
  engine.housekeeping();  // the engine thread performs the cancel-all
  EXPECT_EQ(rig.venue.cancelCount(), 1U);
}

TEST(Engine, ReconcilerThreadDetectsDriftOnItsOwnThreadAndHaltsTrading) {
  EngineConfig cfg;
  cfg.reconcileEveryNs = 20'000'000;  // 20 ms
  cfg.idleSleep = 100us;
  Rig<SteadyClock> rig(cfg);
  OrderThenAct strat(OrderThenAct::Then::kNothing);
  Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, rig.engineConfig);
  engine.start();
  engine.onQuote(quote(1));
  ASSERT_TRUE(waitFor([&] { return rig.oms.liveOrderCount() == 1U; }));
  ASSERT_TRUE(waitFor([&] { return engine.stats().reconciles.load() >= 2U; }));
  EXPECT_EQ(engine.stats().reconcileProblems.load(), 0U);  // healthy so far
  EXPECT_FALSE(rig.kill.tripped());
  {
    std::scoped_lock lock(rig.venue.mu);  // an unexplained position appears at the broker
    rig.venue.positions = {{"00700", 700, 700, 350'000, 350'000}};
  }
  ASSERT_TRUE(waitFor([&] { return rig.kill.tripped(); }));
  // The kill switch trips inside reconcile(); the counter is bumped just after it returns, so it
  // is only eventually consistent with the trip. Wait for it rather than racing it.
  ASSERT_TRUE(waitFor([&] { return engine.stats().reconcileProblems.load() >= 1U; }));
  ASSERT_TRUE(waitFor([&] { return rig.venue.cancelCount() >= 1U; }));  // and orders are pulled
  engine.stop();
}
