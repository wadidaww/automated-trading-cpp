#include <gtest/gtest.h>

#include <set>
#include <sstream>

#include "futu_trader/backtest/runner.hpp"
#include "futu_trader/backtest/synthetic.hpp"
#include "futu_trader/data/event_log.hpp"
#include "futu_trader/portfolio/fees.hpp"
#include "futu_trader/strategy/strategies.hpp"

using namespace futu_trader;
using namespace futu_trader::backtest;
using namespace futu_trader::strategy;

namespace {

std::vector<QuoteEvent> risingQuotes(int n) {
  std::vector<QuoteEvent> out;
  for (int i = 0; i < n; ++i) {
    const Money bid = 350'000 + (200 * i);
    out.push_back({1'767'600'000'000'000'000LL + (static_cast<std::int64_t>(i) * 1'000'000'000LL),
                   "00700", bid, bid + 200, bid, 5000, 5000});
  }
  return out;
}

std::vector<QuoteEvent> synthetic(std::uint64_t seed, std::size_t count, int reversionTicks = 40,
                                  int maxStepTicks = 2) {
  SyntheticConfig cfg;
  cfg.seed = seed;
  cfg.count = count;
  cfg.reversionTicks = reversionTicks;
  cfg.maxStepTicks = maxStepTicks;
  return generateSyntheticQuotes(cfg);
}

MeanReversion::Params meanRevParams() {
  MeanReversion::Params p;
  p.window = 60;
  p.entryZx10 = 15;
  p.exitZx10 = 0;
  p.qty = 100;
  return p;
}

}  // namespace

TEST(Backtest, BuyAndHoldMatchesHandComputation) {
  const auto events = risingQuotes(10);
  BuyAndHold strat("00700", 100);
  const auto result = runBacktest(defaultBacktestConfig(), events, strat);

  ASSERT_EQ(result.fills.size(), 1U);
  EXPECT_EQ(result.fills[0].priceMills, 350'200);  // filled at the first quote's ask
  EXPECT_EQ(result.fills[0].qty, 100);
  EXPECT_EQ(result.finalPositions.at("00700"), 100);
  EXPECT_EQ(result.orders.size(), 1U);

  const Money fee = portfolio::hkFee(portfolio::HkFeeSchedule{}, 100 * 350'200);
  EXPECT_EQ(result.totalFees, fee);
  const Money lastMid = events.back().mid();  // 351,800 + 100
  EXPECT_EQ(result.initialEquity, 1'000'000'000);
  EXPECT_EQ(result.finalEquity, 1'000'000'000 - (100 * 350'200) - fee + (100 * lastMid));
  EXPECT_TRUE(result.finalReconcileClean) << result.firstDrift;
  EXPECT_FALSE(result.killSwitchTripped);

  // Liquidation value closes the position at the BID and pays exit costs, so it is lower than the
  // mid-marked equity: comparing strategies on finalEquity alone flatters ones that hold risk.
  const Money exitFee = portfolio::hkFee(portfolio::HkFeeSchedule{}, 100 * events.back().bid);
  EXPECT_EQ(result.finalLiquidationEquity,
            1'000'000'000 - (100 * 350'200) - fee + (100 * events.back().bid) - exitFee);
  EXPECT_LT(result.finalLiquidationEquity, result.finalEquity);
}

TEST(Backtest, TradeStatisticsAreNetOfFeesSoACostlyWinnerIsNotAWinner) {
  // Review finding: realized P&L excludes fees, so a round trip that gains 20 HKD gross but pays
  // ~80 HKD in fees used to count as a WIN. Buy at ask_0 (350.2), sell at bid_2 (350.4).
  struct RoundTrip final : IStrategy {
    void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override {
      ++n;
      if (n == 1) {
        ctx.submit("00700", Side::kBuy, 100, quote.ask);
      } else if (n == 3 && ctx.position("00700") > 0) {
        ctx.submit("00700", Side::kSell, 100, quote.bid);
      }
    }
    int n{0};
  } strat;
  const auto result = runBacktest(defaultBacktestConfig(), risingQuotes(6), strat);
  ASSERT_EQ(result.fills.size(), 2U);
  const Money gross = (result.fills[1].priceMills - result.fills[0].priceMills) * 100;
  EXPECT_EQ(gross, 20'000);  // +20 HKD before costs
  ASSERT_EQ(result.trades.count, 1U);
  EXPECT_EQ(result.trades.wins, 0U);  // net of ~80 HKD of fees it is a loss
  EXPECT_EQ(result.trades.hitRate, 0.0);
  EXPECT_LT(result.trades.avgLossMills, 0.0);
  EXPECT_EQ(result.finalEquity - result.initialEquity, gross - result.totalFees);
}

TEST(Backtest, IsDeterministicAndSeedSensitive) {
  const auto events = synthetic(42, 5000);
  MeanReversion a(meanRevParams());
  MeanReversion b(meanRevParams());
  const auto first = runBacktest(defaultBacktestConfig(), events, a);
  const auto second = runBacktest(defaultBacktestConfig(), events, b);
  EXPECT_EQ(first.journalHash, second.journalHash);
  EXPECT_EQ(first.finalEquity, second.finalEquity);
  EXPECT_EQ(first.fills.size(), second.fills.size());
  ASSERT_GT(first.fills.size(), 4U) << "the scenario must actually trade to prove anything";

  MeanReversion c(meanRevParams());
  const auto other = runBacktest(defaultBacktestConfig(), synthetic(43, 5000), c);
  EXPECT_NE(first.journalHash, other.journalHash);
}

TEST(Backtest, ReplayingARecordedSessionGivesTheIdenticalResult) {
  const auto events = synthetic(7, 4000);
  std::stringstream log;
  data::EventLogWriter writer(log);
  for (const auto& e : events) {
    ASSERT_TRUE(writer.writeQuote(e));
  }
  const auto loaded = data::readEventLog(log);
  ASSERT_EQ(loaded.status, data::LogStatus::kOk);
  ASSERT_EQ(loaded.quotes.size(), events.size());

  MeanReversion live(meanRevParams());
  MeanReversion replay(meanRevParams());
  const auto original = runBacktest(defaultBacktestConfig(), events, live);
  const auto replayed = runBacktest(defaultBacktestConfig(), loaded.quotes, replay);
  EXPECT_EQ(original.journalHash, replayed.journalHash);
  EXPECT_EQ(original.decisions, replayed.decisions);
  EXPECT_EQ(original.finalEquity, replayed.finalEquity);
}

TEST(Backtest, OmsBooksAndCashAgreeWithTheBrokerAtTheEnd) {
  // The strongest consistency check we have: after thousands of quotes, partial fills, resting
  // orders and periodic reconciliations, our position book, order states and CASH (fees included)
  // still match the simulated broker exactly, and nothing tripped.
  const auto events = synthetic(42, 20'000);
  MeanReversion strat(meanRevParams());
  const auto result = runBacktest(defaultBacktestConfig(), events, strat);
  ASSERT_GT(result.accepted, 10U);
  EXPECT_TRUE(result.finalReconcileClean) << result.firstDrift;
  EXPECT_EQ(result.reconcileDrifts, 0U) << result.firstDrift;
  EXPECT_GT(result.reconciles, 10U);
  EXPECT_FALSE(result.killSwitchTripped) << result.killReason;
}

TEST(Backtest, StrategyOnlySeesEventsUpToNowInOrder) {
  struct Probe final : IStrategy {
    void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override {
      EXPECT_EQ(ctx.nowNs(), quote.tsNs);  // "now" is exactly this quote's time, never later
      if (!seen.empty()) {
        EXPECT_GT(quote.tsNs, seen.back().tsNs);
      }
      seen.push_back(quote);
    }
    std::vector<QuoteEvent> seen;
  } probe;
  const auto events = synthetic(3, 1000);
  runBacktest(defaultBacktestConfig(), events, probe);
  EXPECT_EQ(probe.seen, events);
}

TEST(Backtest, OrdersAreNotFilledBeforeTheyCouldHaveReachedTheMarket) {
  // Decided on quote 0 (t = 0); with 400 ms latency it may not fill before t = 400 ms, and it
  // fills against the market as it is on arrival (still quote 0, as the next quote is at 1 s).
  const auto events = risingQuotes(5);
  BuyAndHold strat("00700", 100);
  auto cfg = defaultBacktestConfig();
  cfg.venue.latencyNs = 400'000'000;
  const auto result = runBacktest(cfg, events, strat);
  ASSERT_EQ(result.fills.size(), 1U);
  ASSERT_EQ(result.fillTimesNs.size(), 1U);
  EXPECT_EQ(result.fills[0].priceMills, events[0].ask);
  EXPECT_EQ(result.fillTimesNs[0] - events[0].tsNs, 400'000'000);
}

TEST(Backtest, LatencyBeyondTheNextQuoteMeansTheMarketMovedAwayAndTheOrderRests) {
  const auto events = risingQuotes(5);  // asks rise by 200 every second and never come back
  BuyAndHold strat("00700", 100);
  auto cfg = defaultBacktestConfig();
  cfg.venue.latencyNs = 1'500'000'000;  // arrives after quotes 0 and 1 have already passed
  const auto result = runBacktest(cfg, events, strat);
  // The limit was quote 0's ask (350.2); on arrival the ask is 350.6, so it cannot fill. A
  // backtester that ignored latency would have filled it at 350.2 and looked better than reality.
  EXPECT_TRUE(result.fills.empty());
  EXPECT_TRUE(result.finalPositions.empty());
  EXPECT_EQ(result.orders.size(), 1U);
  EXPECT_TRUE(result.finalReconcileClean) << result.firstDrift;
}

// --- Lookahead detection and the zero-edge canary -----------------------------------------------

TEST(Lookahead, HonestStrategiesAreConsistentAtEverySampledDecision) {
  const auto events = synthetic(11, 1500);
  const auto cfg = defaultBacktestConfig();
  const auto meanRev = detectLookahead(cfg, events, [](const std::vector<QuoteEvent>&) {
    return std::make_unique<MeanReversion>(meanRevParams());
  });
  EXPECT_TRUE(meanRev.consistent) << meanRev.firstMismatch;
  EXPECT_GT(meanRev.cutsChecked, 1U);  // it must actually have tested something

  const auto random = detectLookahead(cfg, events, [](const std::vector<QuoteEvent>&) {
    return std::make_unique<RandomTrader>("00700", 100, 10);
  });
  EXPECT_TRUE(random.consistent) << random.firstMismatch;
  EXPECT_GT(random.cutsChecked, 1U);

  const auto hold = detectLookahead(cfg, events, [](const std::vector<QuoteEvent>&) {
    return std::make_unique<BuyAndHold>("00700", 100);
  });
  EXPECT_TRUE(hold.consistent) << hold.firstMismatch;
  EXPECT_GE(hold.cutsChecked, 1U);
}

TEST(Lookahead, FuturePeekerIsCaughtBecauseItsPastDecisionsChangeWithoutTheFuture) {
  const auto events = synthetic(11, 1500);
  const auto report =
      detectLookahead(defaultBacktestConfig(), events, [](const std::vector<QuoteEvent>& data) {
        return std::make_unique<FuturePeeker>("00700", 100, &data);  // reads ITS run's data
      });
  EXPECT_FALSE(report.consistent);
  EXPECT_FALSE(report.firstMismatch.empty());
  EXPECT_GE(report.cutsChecked, 1U);
}

TEST(Lookahead, DegenerateInputsAreReportedNotSilentlyPassed) {
  const auto mk = [](const std::vector<QuoteEvent>&) {
    return std::make_unique<BuyAndHold>("00700", 100);
  };
  EXPECT_FALSE(detectLookahead(defaultBacktestConfig(), {}, mk).consistent);
  EXPECT_FALSE(detectLookahead(defaultBacktestConfig(), synthetic(1, 100), mk, 0).consistent);
}

TEST(Lookahead, AStrategyThatNeverTradesTestsNothingAndSaysSo) {
  const auto report = detectLookahead(
      defaultBacktestConfig(), synthetic(1, 200), [](const std::vector<QuoteEvent>&) {
        return std::make_unique<BuyAndHold>("99999", 100);  // a symbol that never appears
      });
  EXPECT_EQ(report.cutsChecked, 0U);
  EXPECT_FALSE(report.consistent);  // "nothing found" must never read as "verified"
  EXPECT_NE(report.firstMismatch.find("cutsChecked == 0"), std::string::npos);
}

TEST(Lookahead, AStrategyThatCheatsOnlyLateInTheDataIsStillCaught) {
  // Review finding: cut sampling stopped short of the last decisions, so a peeker that only
  // cheats in the final tenth of the data slipped through.
  struct LatePeeker final : IStrategy {
    LatePeeker(const std::vector<QuoteEvent>* d) : data(d) {}
    void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override {
      const std::size_t i = index++;
      const bool late = i * 10 >= data->size() * 9;  // the last 10% only
      const Money reference = (late && i + 1 < data->size()) ? (*data)[i + 1].mid() : quote.mid();
      if (ctx.hasLiveOrder("00700")) {
        return;
      }
      const std::int64_t held = ctx.position("00700");
      if (held == 0 && (i % 7 == 0 || reference > quote.mid())) {
        ctx.submit("00700", Side::kBuy, 100, quote.ask);
      } else if (held > 0 && (i % 11 == 0 || reference < quote.mid())) {
        ctx.submit("00700", Side::kSell, held, quote.bid);
      }
    }
    const std::vector<QuoteEvent>* data;
    std::size_t index{0};
  };
  const auto events = synthetic(11, 1500);
  const auto report = detectLookahead(
      defaultBacktestConfig(), events,
      [](const std::vector<QuoteEvent>& data) { return std::make_unique<LatePeeker>(&data); }, 8);
  EXPECT_FALSE(report.consistent) << "cut points must reach the end of the data";
}

TEST(Canary, TheFuturePeekerLooksBrilliantWhichIsExactlyWhyTheDetectorMatters) {
  // With large moves the peeker's knowledge of the next quote easily beats spread and fees.
  const auto events = synthetic(11, 6000, 60, 20);
  FuturePeeker peeker("00700", 100, &events);
  const auto result = runBacktest(defaultBacktestConfig(), events, peeker);
  EXPECT_GT(result.finalEquity, result.initialEquity);  // profit from tomorrow's news
  EXPECT_GT(result.trades.hitRate, 0.6);
  EXPECT_TRUE(result.finalReconcileClean) << result.firstDrift;
}

TEST(Canary, ZeroEdgeRandomTraderLosesAboutTheCostsAcrossSeeds) {
  // Random entries and exits on a random walk have no edge, but crossing the spread and paying
  // HK fees on every round trip is a sure cost. Averaged over many seeds the result must be
  // clearly negative; a backtest that reports profit here has a bug (free fills, lookahead, ...).
  double netSum = 0.0;
  double feeSum = 0.0;
  const int seeds = 20;
  for (int seed = 1; seed <= seeds; ++seed) {
    const auto events = synthetic(static_cast<std::uint64_t>(seed), 10'000, 0);
    RandomTrader trader("00700", 100, 25);
    auto cfg = defaultBacktestConfig();
    cfg.seed = static_cast<std::uint64_t>(seed);
    const auto result = runBacktest(cfg, events, trader);
    ASSERT_TRUE(result.finalReconcileClean) << result.firstDrift;
    netSum += static_cast<double>(result.finalEquity - result.initialEquity);
    feeSum += static_cast<double>(result.totalFees);
  }
  EXPECT_GT(feeSum, 0.0);
  EXPECT_LT(netSum / seeds, 0.0);                      // loses money on average
  EXPECT_LT(netSum / seeds, -(feeSum / seeds) * 0.8);  // and by at least most of the fees paid
  EXPECT_GT(netSum / seeds, -(feeSum / seeds) * 4.0);  // but not absurdly more: a runaway loss is
                                                       // also a sign of a broken simulator
}

// --- Safety machinery under backtest ------------------------------------------------------------

TEST(Backtest, DailyLossLimitHaltsTradingForTheRestOfTheRun) {
  auto cfg = defaultBacktestConfig();
  cfg.limits.maxDailyLossMinor = 300'000;  // 300 HKD
  RandomTrader trader("00700", 100, 1);
  const auto events = synthetic(5, 5000, 0);
  const auto result = runBacktest(cfg, events, trader);
  EXPECT_TRUE(result.killSwitchTripped);
  EXPECT_NE(result.killReason.find("daily loss"), std::string::npos);
  EXPECT_GT(result.riskRejects, 0U);
  EXPECT_TRUE(result.finalReconcileClean) << result.firstDrift;
}

TEST(Backtest, RateLimitRejectsExcessOrdersWithoutSendingThem) {
  auto cfg = defaultBacktestConfig();
  cfg.rate = {.maxPerWindow = 4, .windowMs = 30'000, .reservedForCancels = 0};
  RandomTrader trader("00700", 100, 1);
  const auto events = synthetic(9, 300, 0);  // ~5 minutes at one quote per second
  const auto result = runBacktest(cfg, events, trader);
  EXPECT_GT(result.riskRejects, 0U);
  EXPECT_LE(result.orders.size(), 4U * 11U);  // never more than 4 per 30 s window
  EXPECT_TRUE(result.finalReconcileClean) << result.firstDrift;
}

TEST(Backtest, InvalidOrdersNeverReachTheBrokerBecauseRiskCatchesThemFirst) {
  struct BadLot final : IStrategy {
    void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override {
      if (!done) {
        done = true;
        first = ctx.submit("00700", Side::kBuy, 150, quote.ask).status;  // not a board lot
      }
    }
    bool done{false};
    oms::SubmitStatus first{oms::SubmitStatus::kAccepted};
  } strat;
  const auto result = runBacktest(defaultBacktestConfig(), synthetic(1, 50), strat);
  EXPECT_EQ(strat.first, oms::SubmitStatus::kRejectedByRisk);
  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.venueRejects, 0U);
}

TEST(Backtest, MetricsAreProducedAndSane) {
  const auto events = synthetic(42, 20'000);
  MeanReversion strat(meanRevParams());
  const auto result = runBacktest(defaultBacktestConfig(), events, strat);
  EXPECT_GT(result.perf.periods, 100U);
  EXPECT_GE(result.perf.maxDrawdown, 0.0);
  EXPECT_LT(result.perf.maxDrawdown, 1.0);
  EXPECT_GE(result.exposureFraction, 0.0);
  EXPECT_LE(result.exposureFraction, 1.0);
  EXPECT_GT(result.turnover, 0.0);
  EXPECT_GT(result.trades.count, 3U);
  EXPECT_FALSE(result.perf.ruined);
}

TEST(Backtest, EmptyDataIsHandled) {
  BuyAndHold strat("00700", 100);
  const auto result = runBacktest(defaultBacktestConfig(), {}, strat);
  EXPECT_TRUE(result.fills.empty());
  EXPECT_EQ(result.submits, 0U);
}

// --- Review findings: data handling, multi-symbol, annualisation, lifecycle ---------------------

TEST(Backtest, InvalidMarketDataIsRefusedNotReplayed) {
  BuyAndHold strat("00700", 100);
  auto backwards = risingQuotes(4);
  std::swap(backwards[1], backwards[2]);  // time goes backwards
  auto crossed = risingQuotes(4);
  crossed[2].bid = crossed[2].ask + 200;
  auto negative = risingQuotes(4);
  negative[1].bid = -5;
  for (const auto* bad : {&backwards, &crossed, &negative}) {
    const auto result = runBacktest(defaultBacktestConfig(), *bad, strat);
    EXPECT_FALSE(result.dataError.empty());
    EXPECT_TRUE(result.fills.empty());
    EXPECT_EQ(result.submits, 0U);
  }
}

TEST(Backtest, QuotesSharingATimestampAreDeliveredInOrderAndDeterministically) {
  struct Probe final : IStrategy {
    void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override {
      EXPECT_EQ(ctx.nowNs(), quote.tsNs);
      bids.push_back(quote.bid);
    }
    std::vector<Money> bids;
  };
  auto events = risingQuotes(5);
  events[3].tsNs = events[2].tsNs;  // two quotes at the same instant
  Probe first;
  Probe second;
  runBacktest(defaultBacktestConfig(), events, first);
  runBacktest(defaultBacktestConfig(), events, second);
  ASSERT_EQ(first.bids.size(), 5U);
  EXPECT_EQ(first.bids, second.bids);
  EXPECT_EQ(first.bids[2], events[2].bid);
  EXPECT_EQ(first.bids[3], events[3].bid);
}

TEST(Backtest, TwoSymbolsAreTradedAndReconciledIndependently) {
  // The runner used to read only "00700" for positions and exposure.
  struct BuyBoth final : IStrategy {
    void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override {
      if (bought.insert(quote.symbol).second) {
        ctx.submit(quote.symbol, Side::kBuy, 100, quote.ask);
      }
    }
    std::set<std::string> bought;
  } strat;
  std::vector<QuoteEvent> events;
  for (int i = 0; i < 6; ++i) {
    const std::int64_t ts = 1'767'600'000'000'000'000LL + (i * 1'000'000'000LL);
    events.push_back({ts, "00700", 350'000, 350'200, 350'000, 5000, 5000});
    events.push_back({ts, "09988", 80'000, 80'050, 80'000, 5000, 5000});
  }
  auto cfg = defaultBacktestConfig();
  cfg.instruments = {{"00700", 100}, {"09988", 100}};
  const auto result = runBacktest(cfg, events, strat);
  ASSERT_EQ(result.finalPositions.size(), 2U);
  EXPECT_EQ(result.finalPositions.at("00700"), 100);
  EXPECT_EQ(result.finalPositions.at("09988"), 100);
  EXPECT_TRUE(result.finalReconcileClean) << result.firstDrift;
  EXPECT_EQ(result.reconcileDrifts, 0U);
  EXPECT_GT(result.exposureFraction, 0.0);
}

TEST(Backtest, AnnualisationFollowsTheSamplingPeriodUnlessOverridden) {
  BuyAndHold strat("00700", 100);
  auto cfg = defaultBacktestConfig();
  cfg.equitySampleNs = 60'000'000'000LL;
  EXPECT_DOUBLE_EQ(runBacktest(cfg, risingQuotes(3), strat).periodsPerYear, 250.0 * 19'800 / 60.0);
  BuyAndHold again("00700", 100);
  cfg.equitySampleNs = 1'000'000'000LL;  // one-second samples: 60x more periods per year
  EXPECT_DOUBLE_EQ(runBacktest(cfg, risingQuotes(3), again).periodsPerYear, 250.0 * 19'800);
  BuyAndHold third("00700", 100);
  cfg.periodsPerYear = 252.0;  // an explicit value wins
  EXPECT_DOUBLE_EQ(runBacktest(cfg, risingQuotes(3), third).periodsPerYear, 252.0);
}

TEST(Backtest, PassiveOrdersExerciseRestingPartialFillCancelAndRaceLifecycles) {
  // The golden runs of taker strategies fill every order whole at the signal quote, so they never
  // touch these paths. The passive maker rests orders, sees partial fills against a small
  // displayed size, and cancels stale orders; the OMS must still end up matching the broker.
  SyntheticConfig cfg;
  cfg.seed = 11;
  cfg.count = 20'000;
  cfg.size = 200;  // less than the 300-share orders: forces partial fills
  cfg.maxStepTicks = 3;
  const auto events = generateSyntheticQuotes(cfg);
  PassiveMaker maker(PassiveMaker::Params{});
  auto config = defaultBacktestConfig();
  config.limits.maxDailyLossMinor = 900'000'000;  // let it run the whole dataset: this test is
                                                  // about lifecycle paths, not the loss limit
  const auto result = runBacktest(config, events, maker);

  std::size_t cancelled = 0;
  std::size_t partialThenCancelled = 0;
  std::size_t filled = 0;
  for (const auto& rec : result.orders) {
    cancelled += rec.state == oms::OmsState::kCancelled ? 1U : 0U;
    filled += rec.state == oms::OmsState::kFilled ? 1U : 0U;
    partialThenCancelled += (rec.state == oms::OmsState::kCancelled && rec.filledQty > 0) ? 1U : 0U;
  }
  EXPECT_GT(cancelled, 5U);
  EXPECT_GT(filled, 5U);
  EXPECT_GT(result.fills.size(), result.orders.size() - cancelled);  // some orders filled in parts
  EXPECT_TRUE(result.finalReconcileClean) << result.firstDrift;
  EXPECT_EQ(result.reconcileDrifts, 0U) << result.firstDrift;
  EXPECT_FALSE(result.killSwitchTripped) << result.killReason;
  (void)partialThenCancelled;
}
