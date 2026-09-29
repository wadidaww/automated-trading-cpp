#pragma once

#include <vector>

#include "futu_trader/core/types.hpp"
#include "futu_trader/model/signal_model.hpp"

namespace futu_trader {

struct BacktestConfig {
  double periodsPerYear{252.0};  // bars per year, used only for annualization
  double feeBps{0.0};            // cost per unit of position change, in basis points
};

struct BacktestMetrics {
  double totalReturn{0.0};
  double annualizedReturn{0.0};
  double annualizedVolatility{0.0};
  double sharpe{0.0};
  double maxDrawdown{0.0};
  double winRate{0.0};  // fraction of in-market bars with positive P&L
  std::size_t trades{0};
};

/**
 * Bar-by-bar replay. The signal computed from the return of bar i-1 is applied to the return of
 * bar i, so the strategy never trades on information it could not have had.
 */
class Backtester {
 public:
  BacktestMetrics run(const std::vector<Tick>& ticks, const ISignalModel& model,
                      BacktestConfig config = {}) const;
};

}  // namespace futu_trader
