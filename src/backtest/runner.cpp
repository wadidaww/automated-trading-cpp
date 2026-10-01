#include "futu_trader/backtest/runner.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>

#include "futu_trader/backtest/synthetic.hpp"
#include "futu_trader/data/validate.hpp"
#include "futu_trader/execution/kill_switch.hpp"
#include "futu_trader/portfolio/fees.hpp"
#include "futu_trader/portfolio/position_book.hpp"

namespace futu_trader::backtest {

namespace {

__extension__ using Int128 = __int128;

constexpr std::int64_t kHkOffsetNs = 8LL * 3600 * 1'000'000'000;
constexpr std::int64_t kDayNs = 24LL * 3600 * 1'000'000'000;

class Fnv {
 public:
  void add(const std::string& text) {
    for (const char c : text) {
      byte(static_cast<std::uint8_t>(c));
    }
    byte(0xFF);
  }
  void add(std::int64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
      byte(static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (8U * i)));
    }
  }
  std::uint64_t value() const { return hash_; }

 private:
  void byte(std::uint8_t b) {
    hash_ ^= b;
    hash_ *= 0x100000001B3ULL;
  }
  std::uint64_t hash_{0xCBF29CE484222325ULL};
};

// Closed-trade P&L NET of fees. PositionBook's realized P&L is gross (fees only touch cash), so
// a strategy that loses money after costs would otherwise show a flattering hit rate. Entry fees
// are carried per symbol and released pro rata as the position is closed.
class TradeTracker {
 public:
  void onFill(const opend::BrokerFill& fill, Money fee) {
    const std::int64_t before = book_.qty(fill.code);
    const Money realizedBefore = book_.totalRealizedPnl();
    (void)book_.applyFill({fill.fillId, fill.code, fill.side, fill.qty, fill.priceMills, fee});
    const Money realizedDelta = book_.totalRealizedPnl() - realizedBefore;

    Money& entryFees = entryFees_[fill.code];
    const std::int64_t absBefore = std::llabs(before);
    const bool closes = absBefore > 0 && ((before > 0) == (fill.side == Side::kSell));
    if (!closes) {
      entryFees += fee;  // opening or adding: this fee belongs to the trade being built
      return;
    }
    const std::int64_t closeQty = std::min(absBefore, fill.qty);
    const auto closeFee = static_cast<Money>(static_cast<Int128>(fee) * closeQty / fill.qty);
    const Money openFee = fee - closeFee;  // non-zero only when the fill crosses through zero
    const auto entryPortion =
        static_cast<Money>(static_cast<Int128>(entryFees) * closeQty / absBefore);
    closed_.push_back(realizedDelta - closeFee - entryPortion);
    entryFees = entryFees - entryPortion + openFee;
    if (book_.qty(fill.code) == 0) {
      entryFees = 0;  // flat: drop any rounding remainder
    }
  }
  const std::vector<Money>& closedTrades() const { return closed_; }

 private:
  portfolio::PositionBook book_;
  std::map<std::string, Money> entryFees_;
  std::vector<Money> closed_;
};

// The strategy's window onto the world: everything goes through the OMS.
class Context final : public strategy::StrategyContext {
 public:
  Context(oms::Oms& oms, const portfolio::PositionBook& book, const ManualClock& clock,
          const std::map<std::string, QuoteEvent>& quotes, std::uint64_t seed,
          std::vector<std::string>& decisions, BacktestResult& result)
      : oms_(oms),
        book_(book),
        clock_(clock),
        quotes_(quotes),
        rng_(seed),
        decisions_(decisions),
        result_(result) {}

  std::int64_t nowNs() const override { return clock_.nowNs(); }
  std::int64_t position(const std::string& symbol) const override { return book_.qty(symbol); }

  bool hasLiveOrder(const std::string& symbol) const override {
    return !liveOrderIds(symbol).empty();
  }
  std::vector<std::string> liveOrderIds(const std::string& symbol) const override {
    return oms_.liveOrderIds(symbol);
  }

  oms::SubmitResult submit(const std::string& symbol, Side side, std::int64_t qty,
                           Money priceMills) override {
    decisions_.push_back(std::to_string(clock_.nowNs()) + " " + symbol +
                         (side == Side::kBuy ? " B " : " S ") + std::to_string(qty) + " " +
                         std::to_string(priceMills));
    oms::QuoteContext quote;
    const auto found = quotes_.find(symbol);
    if (found != quotes_.end()) {
      quote.lastPriceMills = found->second.mid();
      quote.quoteTimeNs = found->second.tsNs;
    }
    quote.nowNs = clock_.nowNs();
    oms::OrderIntent intent{"S" + std::to_string(++counter_), symbol, side, qty, priceMills};
    const auto outcome = oms_.submit(intent, quote);
    ++result_.submits;
    switch (outcome.status) {
      case oms::SubmitStatus::kAccepted:
        ++result_.accepted;
        break;
      case oms::SubmitStatus::kRejectedByRisk:
        ++result_.riskRejects;
        break;
      case oms::SubmitStatus::kRejectedByVenue:
        ++result_.venueRejects;
        break;
      default:
        break;
    }
    return outcome;
  }

  Result<bool> cancel(const std::string& clOrdId) override { return oms_.cancel(clOrdId); }
  std::uint64_t random() override { return rng_.next(); }

 private:
  oms::Oms& oms_;
  const portfolio::PositionBook& book_;
  const ManualClock& clock_;
  const std::map<std::string, QuoteEvent>& quotes_;
  Rng rng_;
  std::vector<std::string>& decisions_;
  BacktestResult& result_;
  std::uint64_t counter_{0};
};

}  // namespace

BacktestConfig defaultBacktestConfig() {
  BacktestConfig cfg;
  cfg.oms.sessionEpoch = "BT";
  cfg.oms.cashToleranceMills = 0;  // reconciliation also proves our cash equals the broker's
  cfg.limits = {.maxPositionNotionalMinor = 500'000'000,   // 500,000 HKD per symbol
                .maxPortfolioNotionalMinor = 900'000'000,  // 900,000 HKD gross
                .maxDailyLossMinor = 100'000'000,          // 100,000 HKD
                .maxOpenOrders = 20,
                .concentrationLimit = 1.0};
  cfg.preTrade = {.priceBandBps = 500,
                  .maxQuoteAgeMs = 5000,
                  .maxOrderNotionalMills = 500'000'000,
                  .allowShort = false};
  cfg.rate = {.maxPerWindow = 100000, .windowMs = 30'000, .reservedForCancels = 1000};
  cfg.instruments = {{"00700", 100}};
  cfg.venue.sizeZeroMeansUnlimited = false;  // unknown displayed size is not evidence of liquidity
  return cfg;
}

BacktestResult runBacktest(const BacktestConfig& config, const std::vector<QuoteEvent>& events,
                           strategy::IStrategy& strat) {
  BacktestResult result;
  const auto valid = data::validateQuotes(events);
  if (!valid) {
    result.dataError = valid.error().message;  // refuse to run on data that does not make sense
    return result;
  }
  if (config.periodsPerYear > 0.0) {
    result.periodsPerYear = config.periodsPerYear;
  } else if (config.equitySampleNs > 0) {
    result.periodsPerYear = static_cast<double>(config.tradingDaysPerYear) * config.sessionSeconds /
                            (static_cast<double>(config.equitySampleNs) / 1e9);
  }
  ManualClock clock;
  execution::KillSwitch kill;
  instrument::InstrumentTable instruments;
  for (const auto& info : config.instruments) {
    instruments.add(info);
  }
  portfolio::PositionBook book;
  SimVenue venue(config.venue, instruments, clock);
  execution::RateLimiter rate(config.rate, clock);
  oms::PreTradeRisk risk(config.limits, config.preTrade, kill, instruments);
  // The OMS computes fees for its own books; they must be the schedule the broker charges, or the
  // cash reconciliation would flag a phantom drift.
  oms::OmsConfig omsConfig = config.oms;
  omsConfig.fees = config.venue.fees;
  oms::Oms oms(venue, risk, rate, kill, book, clock, omsConfig);

  // Reporting only: closed-trade P&L net of fees, and traded notional.
  TradeTracker tracker;
  Money fillNotional = 0;

  venue.setSinks([&](const opend::BrokerOrder& o) { oms.onOrderUpdate(o); },
                 [&](const opend::BrokerFill& f) {
                   result.fillTimesNs.push_back(clock.nowNs());
                   oms.onFill(f);
                   const Int128 turnover = static_cast<Int128>(f.priceMills) * f.qty;
                   tracker.onFill(
                       f, portfolio::hkFee(config.venue.fees, static_cast<Money>(turnover)));
                   fillNotional += static_cast<Money>(turnover);
                 });

  std::map<std::string, QuoteEvent> quotes;
  std::map<std::string, Money> marks;
  // The strategy's random stream is separate from any data-generation stream, so seeding both
  // with the same number cannot correlate the trader's coin flips with the price path.
  Context ctx(oms, book, clock, quotes, config.seed ^ 0xD1B54A32D192ED03ULL, result.decisions,
              result);

  if (!oms.bootstrap()) {
    result.killReason = "bootstrap failed";
    return result;
  }
  result.initialEquity = venue.equity({});

  std::int64_t nextReconcile = 0;
  std::int64_t nextSample = 0;
  std::int64_t currentDay = -1;
  std::size_t samplesWithPosition = 0;
  Int128 equitySum = 0;

  const auto sampleEquity = [&](std::int64_t ts) {
    const Money equity = venue.equity(marks);
    result.equity.push_back({ts, equity});
    equitySum += equity;
    bool open = false;
    for (const auto& pos : venue.book().all()) {
      open = open || pos.qty != 0;
    }
    samplesWithPosition += open ? 1U : 0U;
  };

  for (const auto& quote : events) {
    const std::int64_t ts = quote.tsNs;
    // 1) Things scheduled to happen before this quote happen against the OLD market.
    venue.advance(ts - 1);
    clock.setNs(ts);
    // 2) The new quote arrives: resting orders may fill.
    venue.onQuote(quote);
    quotes[quote.symbol] = quote;
    marks[quote.symbol] = quote.mid();
    oms.onMark(quote.symbol, quote.mid());

    const std::int64_t day = (ts + kHkOffsetNs) / kDayNs;
    if (day != currentDay) {
      if (currentDay != -1) {
        oms.resetDailyBaseline();
      }
      currentDay = day;
    }
    if (config.reconcileEveryNs > 0 && ts >= nextReconcile) {
      const auto report = oms.reconcile();
      ++result.reconciles;
      if (!report.clean() && result.firstDrift.empty()) {
        result.firstDrift = report.drifts.empty() ? report.error : report.drifts.front().detail;
      }
      result.reconcileDrifts += report.drifts.size();
      nextReconcile = ts + config.reconcileEveryNs;
    }
    oms.serviceHalt();

    // 3) The strategy sees this quote (and nothing later) and may act.
    strat.onQuote(quote, ctx);
    venue.advance(ts);  // zero-latency orders take effect now

    if (config.equitySampleNs > 0 && ts >= nextSample) {
      sampleEquity(ts);
      nextSample = ts + config.equitySampleNs;
    }
  }

  // Let anything still in flight land, using the last market we saw.
  if (!events.empty()) {
    const std::int64_t end =
        events.back().tsNs + config.venue.latencyNs + config.venue.cancelLatencyNs + 1;
    venue.advance(end);
    clock.setNs(std::max(clock.nowNs(), end));
    oms.serviceHalt();
    const auto report = oms.reconcile();
    ++result.reconciles;
    result.finalReconcileClean = report.clean();
    if (!report.clean() && result.firstDrift.empty()) {
      result.firstDrift = report.drifts.empty() ? report.error : report.drifts.front().detail;
    }
    result.reconcileDrifts += report.drifts.size();
    sampleEquity(events.back().tsNs);
  }

  result.fills = venue.fills();
  result.orders = oms.orders();
  result.totalFees = venue.totalFees();
  result.finalEquity = venue.equity(marks);
  for (const auto& pos : venue.book().all()) {
    if (pos.qty != 0) {
      result.finalPositions[pos.symbol] = pos.qty;
    }
  }
  // Liquidation value: close everything at the last touch and pay the exit costs.
  {
    Int128 liquidation = venue.cash();
    for (const auto& pos : venue.book().all()) {
      if (pos.qty == 0) {
        continue;
      }
      const auto quote = quotes.find(pos.symbol);
      Money touch = pos.costBasis / std::llabs(pos.qty);  // no quote: value at cost
      if (quote != quotes.end()) {
        touch = pos.qty > 0 ? quote->second.bid : quote->second.ask;  // bid to sell, ask to cover
      }
      const Int128 gross = static_cast<Int128>(pos.qty) * touch;
      const Int128 notional = gross < 0 ? -gross : gross;
      liquidation += gross - portfolio::hkFee(config.venue.fees, static_cast<Money>(notional));
    }
    result.finalLiquidationEquity = static_cast<Money>(liquidation);
  }
  result.killSwitchTripped = kill.tripped();
  result.killReason = kill.reason();

  const auto resampled = resampleEquity(result.equity, config.equitySampleNs);
  result.perf = computeMetrics(resampled, result.periodsPerYear);
  result.periodReturns = returnsFromEquity(resampled);
  result.trades = computeTradeStats(tracker.closedTrades());
  if (!result.equity.empty()) {
    const auto samples = static_cast<double>(result.equity.size());
    result.exposureFraction = static_cast<double>(samplesWithPosition) / samples;
    const double avgEquity = static_cast<double>(equitySum) / samples;
    result.turnover = avgEquity > 0.0 ? static_cast<double>(fillNotional) / avgEquity : 0.0;
  }

  Fnv hash;
  for (const auto& entry : oms.journal()) {
    hash.add(entry.tsNs);
    hash.add(static_cast<std::int64_t>(entry.kind));
    hash.add(entry.clOrdId);
    hash.add(entry.detail);
  }
  for (const auto& fill : result.fills) {
    hash.add(fill.fillId);
    hash.add(fill.orderId > 0 ? static_cast<std::int64_t>(fill.orderId) : 0);
    hash.add(fill.qty);
    hash.add(fill.priceMills);
  }
  hash.add(result.finalEquity);
  result.journalHash = hash.value();
  return result;
}

LookaheadReport detectLookahead(const BacktestConfig& config, const std::vector<QuoteEvent>& events,
                                const StrategyFactory& makeStrategy, std::size_t maxCuts) {
  LookaheadReport report;
  if (events.size() < 2 || maxCuts == 0) {
    report.consistent = false;
    report.firstMismatch = "need at least two events and one cut";
    return report;
  }
  auto fullStrategy = makeStrategy(events);
  const auto full = runBacktest(config, events, *fullStrategy);

  // Decision timestamps -> event indices. A decision made at event c that depends on anything
  // after c changes when the data is cut right after c, so cut right after sampled decisions.
  std::vector<std::size_t> decisionEvents;
  for (const auto& line : full.decisions) {
    const std::int64_t ts = std::stoll(line.substr(0, line.find(' ')));
    const auto found =
        std::lower_bound(events.begin(), events.end(), ts,
                         [](const QuoteEvent& e, std::int64_t t) { return e.tsNs < t; });
    if (found != events.end() && found->tsNs == ts) {
      const auto idx = static_cast<std::size_t>(found - events.begin());
      if (decisionEvents.empty() || decisionEvents.back() != idx) {
        decisionEvents.push_back(idx);
      }
    }
  }
  std::vector<std::size_t> cuts;
  if (!decisionEvents.empty()) {
    // Evenly spaced across ALL decisions, first and last included: a strategy that only cheats
    // late in the data must not slip past a sampling scheme that stops short of the end.
    const std::size_t k = std::min(maxCuts, decisionEvents.size());
    for (std::size_t i = 0; i < k; ++i) {
      const std::size_t pick =
          k == 1 ? decisionEvents.size() - 1 : (i * (decisionEvents.size() - 1)) / (k - 1);
      const std::size_t cut = decisionEvents[pick] + 1;
      if (cut < events.size() && (cuts.empty() || cuts.back() != cut)) {
        cuts.push_back(cut);
      }
    }
  }

  for (const std::size_t cut : cuts) {
    const std::vector<QuoteEvent> prefix(events.begin(),
                                         events.begin() + static_cast<std::ptrdiff_t>(cut));
    auto prefixStrategy = makeStrategy(prefix);
    const auto part = runBacktest(config, prefix, *prefixStrategy);
    ++report.cutsChecked;

    // Decisions made at or before the cut must not depend on anything after it.
    const std::int64_t cutTs = prefix.back().tsNs;
    std::vector<std::string> fullUpToCut;
    for (const auto& line : full.decisions) {
      if (std::stoll(line.substr(0, line.find(' '))) <= cutTs) {
        fullUpToCut.push_back(line);
      }
    }
    report.comparedDecisions += fullUpToCut.size();
    const std::size_t common = std::min(fullUpToCut.size(), part.decisions.size());
    for (std::size_t i = 0; i < common; ++i) {
      if (fullUpToCut[i] != part.decisions[i]) {
        report.consistent = false;
        report.firstMismatch = "cut at event " + std::to_string(cut) + ", decision " +
                               std::to_string(i) + ": full-data run '" + fullUpToCut[i] +
                               "' vs truncated run '" + part.decisions[i] + "'";
        return report;
      }
    }
    if (fullUpToCut.size() != part.decisions.size()) {
      report.consistent = false;
      report.firstMismatch = "cut at event " + std::to_string(cut) + ": the full-data run made " +
                             std::to_string(fullUpToCut.size()) +
                             " decisions up to the cut, the truncated run " +
                             std::to_string(part.decisions.size());
      return report;
    }
  }
  if (report.cutsChecked == 0) {
    // Nothing was tested. "No mismatch found" would read as a pass, so it must not be one.
    report.consistent = false;
    report.firstMismatch = "no decision could be tested (cutsChecked == 0)";
  }
  return report;
}

}  // namespace futu_trader::backtest
