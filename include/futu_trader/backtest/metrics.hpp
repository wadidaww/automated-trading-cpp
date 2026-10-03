#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "futu_trader/core/types.hpp"

namespace futu_trader::backtest {

struct EquityPoint {
  std::int64_t tsNs{0};
  Money equityMills{0};
};

/**
 * One equity value per period that actually contained data: the last sample inside each
 * `periodNs`-wide bucket (aligned to the first sample). Empty buckets (overnight, weekends, the
 * lunch break, quiet spells) are SKIPPED, not carried forward: carrying them forward would invent
 * thousands of zero-return periods, inflate the period count and depress volatility. The return
 * across a gap therefore counts as one observation, which is what actually happened.
 */
std::vector<Money> resampleEquity(const std::vector<EquityPoint>& points, std::int64_t periodNs);

struct PerformanceMetrics {
  std::size_t periods{0};  // number of return observations
  double totalReturn{0.0};
  double annualizedReturn{0.0};       // geometric
  double annualizedVol{0.0};          // sample stddev * sqrt(periodsPerYear)
  double sharpe{0.0};                 // risk-free rate 0; sample stddev
  double sortino{0.0};                // downside deviation over all periods
  double calmar{0.0};                 // annualized return / max drawdown
  double maxDrawdown{0.0};            // fraction of the running peak
  std::size_t maxDrawdownPeriods{0};  // longest time below a prior peak (still open counts)
  bool ruined{false};                 // equity reached zero or below: ratios are meaningless
  /**
   * False when there are too few return observations for the ratios above to mean anything
   * (a Sharpe from a few dozen periods is mostly noise, and annualising a short sample explodes
   * it). Report the ratios only alongside this flag and a confidence interval.
   */
  bool ratiosReliable{false};
};

/** Return observations below which the ratios are flagged unreliable. */
inline constexpr std::size_t kMinReliablePeriods = 250;

/** Metrics from an equity curve sampled once per period. Needs at least two points. */
PerformanceMetrics computeMetrics(const std::vector<Money>& equity, double periodsPerYear);

/** Simple period returns, e_i / e_{i-1} - 1 (empty if any equity is <= 0). */
std::vector<double> returnsFromEquity(const std::vector<Money>& equity);

/** Annualized Sharpe of a return series (0 if fewer than 2 points or zero variance). */
double sharpeOf(const std::vector<double>& returns, double periodsPerYear);

struct TradeStats {
  std::size_t count{0};
  std::size_t wins{0};
  std::size_t losses{0};
  double hitRate{0.0};
  double avgWinMills{0.0};
  double avgLossMills{0.0};  // negative number
  /** Gross wins / gross losses. NaN when undefined (no losing trades): never a misleading 0. */
  double profitFactor{0.0};
};

/** Statistics over realised P&L per closed trade. */
TradeStats computeTradeStats(const std::vector<Money>& closedTradePnl);

struct ConfidenceInterval {
  double lo{0.0};
  double hi{0.0};
  bool valid{false};  // false if it could not be computed: lo/hi are then meaningless
};

/**
 * Bootstrap (resampling with replacement) confidence interval for the annualized Sharpe ratio.
 * Deterministic for a given seed. Assumes roughly independent returns: autocorrelated returns
 * make this interval too narrow.
 */
ConfidenceInterval bootstrapSharpeCi(const std::vector<double>& returns, double periodsPerYear,
                                     std::size_t resamples, std::uint64_t seed,
                                     double alpha = 0.05);

}  // namespace futu_trader::backtest
