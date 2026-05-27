#include "futu_trader/evaluation/backtester.hpp"

#include <algorithm>
#include <cmath>

namespace futu_trader {

BacktestMetrics Backtester::run(const std::vector<Tick>& ticks, const ISignalModel& model) const {
  BacktestMetrics m;
  if (ticks.size() < 2) {
    return m;
  }
  double equity = 1.0;
  double peak = 1.0;
  std::size_t wins = 0;
  for (std::size_t i = 1; i < ticks.size(); ++i) {
    if (ticks[i - 1].priceMinor == 0) {
      continue;
    }
    const double ret = static_cast<double>(ticks[i].priceMinor - ticks[i - 1].priceMinor) /
                       static_cast<double>(ticks[i - 1].priceMinor);
    const Signal s = model.predict({ret});
    double pnl = 0.0;
    if (s.action == SignalAction::kBuy) {
      pnl = ret;
    } else if (s.action == SignalAction::kSell) {
      pnl = -ret;
    }
    if (pnl > 0) {
      ++wins;
    }
    equity *= (1.0 + pnl);
    peak = std::max(peak, equity);
    if (peak > 0.0) {
      m.maxDrawdown = std::max(m.maxDrawdown, (peak - equity) / peak);
    }
  }
  m.totalReturn = equity - 1.0;
  m.annualizedReturn = m.totalReturn;
  m.annualizedVolatility = std::fabs(m.totalReturn);
  m.sharpe = (m.annualizedVolatility == 0.0) ? 0.0 : m.annualizedReturn / m.annualizedVolatility;
  m.winRate = static_cast<double>(wins) / static_cast<double>(ticks.size() - 1);
  return m;
}

}  // namespace futu_trader
