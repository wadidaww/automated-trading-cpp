#pragma once

#include <vector>

#include "futu_trader/core/types.hpp"
#include "futu_trader/model/signal_model.hpp"

namespace futu_trader {

struct BacktestMetrics {
  double totalReturn{0.0};
  double annualizedReturn{0.0};
  double annualizedVolatility{0.0};
  double sharpe{0.0};
  double maxDrawdown{0.0};
  double winRate{0.0};
};

class Backtester {
 public:
  BacktestMetrics run(const std::vector<Tick>& ticks, const ISignalModel& model) const;
};

}  // namespace futu_trader
