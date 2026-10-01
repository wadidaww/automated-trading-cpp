#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "futu_trader/backtest/metrics.hpp"
#include "futu_trader/backtest/sim_venue.hpp"
#include "futu_trader/execution/rate_limiter.hpp"
#include "futu_trader/market/quote.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/oms/pre_trade.hpp"
#include "futu_trader/risk/risk_engine.hpp"
#include "futu_trader/strategy/strategy.hpp"

namespace futu_trader::backtest {

struct BacktestConfig {
  SimVenueConfig venue;
  oms::OmsConfig oms;
  RiskConfig limits;
  oms::PreTradeConfig preTrade;
  execution::RateLimitConfig rate;
  std::vector<instrument::InstrumentInfo> instruments;
  std::int64_t reconcileEveryNs{60'000'000'000LL};  // OMS-vs-broker check cadence (0 = never)
  std::int64_t equitySampleNs{60'000'000'000LL};    // equity sampling period for metrics
  /**
   * Sample periods per year for annualising ratios. 0 (default) derives it from `equitySampleNs`,
   * `tradingDaysPerYear` and `sessionSeconds`, so changing the sampling period can never leave
   * Sharpe annualised for the wrong frequency.
   */
  double periodsPerYear{0.0};
  int tradingDaysPerYear{250};
  int sessionSeconds{19'800};  // HKEX continuous trading: 09:30-12:00 and 13:00-16:00 = 5.5 h
  std::uint64_t seed{1};  // seeds the strategy's random() stream (independent of any data seed)
};

/** Sensible defaults: 1M HKD, one instrument (00700, lot 100), limits that rarely bind. */
BacktestConfig defaultBacktestConfig();

struct BacktestResult {
  std::string dataError;       // non-empty: the market data was invalid and nothing was run
  double periodsPerYear{0.0};  // the annualisation factor actually used
  PerformanceMetrics perf;
  TradeStats trades;
  std::vector<EquityPoint> equity;
  std::vector<double> periodReturns;  // per-sample-period returns behind `perf` (for bootstrap CIs)
  std::vector<opend::BrokerFill> fills;
  std::vector<std::int64_t> fillTimesNs;  // when each fill reached the OMS (parallel to `fills`)
  std::vector<oms::OrderRecord> orders;
  /** One line per strategy order request (time, symbol, side, qty, price); used for lookahead
   * checks. */
  std::vector<std::string> decisions;
  std::uint64_t journalHash{0};  // fingerprint of everything the OMS did; equal runs => equal hash
  Money initialEquity{0};
  Money finalEquity{0};  // positions marked at the mid: flatters an open position
  /**
   * Equity if every open position were closed at the last touch (bid for longs, ask for shorts),
   * after exit fees. Comparing strategies by finalEquity alone favours ones that end in cash.
   */
  Money finalLiquidationEquity{0};
  Money totalFees{0};
  std::map<std::string, std::int64_t> finalPositions;  // every symbol with an open position
  double exposureFraction{0.0};                        // share of samples with an open position
  double turnover{0.0};                                // traded notional / average equity
  std::size_t submits{0};
  std::size_t accepted{0};
  std::size_t riskRejects{0};
  std::size_t venueRejects{0};
  std::size_t reconciles{0};
  std::size_t reconcileDrifts{0};
  std::string firstDrift;
  bool finalReconcileClean{false};
  bool killSwitchTripped{false};
  std::string killReason;
};

/**
 * Replays `events` (which must be sorted by time) through the REAL OMS, risk chain, rate limiter
 * and position book against a simulated broker. The strategy is called once per quote, in order,
 * and can only act through the OMS, so a backtest exercises the same safety code as live trading.
 * Deterministic: identical inputs give an identical result and journal hash.
 */
BacktestResult runBacktest(const BacktestConfig& config, const std::vector<QuoteEvent>& events,
                           strategy::IStrategy& strategy);

struct LookaheadReport {
  bool consistent{true};
  std::size_t cutsChecked{0};  // 0 means nothing was tested (e.g. the strategy never traded)
  std::size_t comparedDecisions{0};
  std::string firstMismatch;
};

using StrategyFactory =
    std::function<std::unique_ptr<strategy::IStrategy>(const std::vector<QuoteEvent>& data)>;

/**
 * Prefix-consistency test for lookahead bias. It runs a strategy over the full data, then for up
 * to `maxCuts` sampled decisions it truncates the data right after that decision, runs a fresh
 * strategy on the truncated data, and requires identical decisions up to the cut. A strategy
 * whose past decisions change when the future is removed was using the future.
 *
 * Cuts are placed right after real decisions because that is where a dependence on later data
 * shows up: a strategy that peeks k quotes ahead changes only its last k decisions before a cut.
 *
 * The factory receives the dataset of the run it builds for. That matters: the typical lookahead
 * bug is a strategy that preloads or precomputes over the data it is run on (whole-series
 * indicators, "next bar" lookups), and it only shows up if the truncated run truly has no future.
 * Honest strategies ignore the argument.
 *
 * This detects the bias; it cannot prove its absence. `cutsChecked == 0` means nothing was tested.
 */
LookaheadReport detectLookahead(const BacktestConfig& config, const std::vector<QuoteEvent>& events,
                                const StrategyFactory& makeStrategy, std::size_t maxCuts = 8);

}  // namespace futu_trader::backtest
