#pragma once

#include <vector>

#include "futu_trader/core/types.hpp"
#include "futu_trader/model/signal_model.hpp"

namespace futu_trader {

struct BacktestMetrics {
  double total_return{0.0};
  double annualized_return{0.0};
  double annualized_volatility{0.0};
  double sharpe{0.0};
  double max_drawdown{0.0};
  double win_rate{0.0};
};

class Backtester {
 public:
  BacktestMetrics Run(const std::vector<Tick>& ticks, const ISignalModel& model) const;
};

}  // namespace futu_trader
