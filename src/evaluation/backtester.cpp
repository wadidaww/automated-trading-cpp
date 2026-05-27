#include "futu_trader/evaluation/backtester.hpp"

#include <algorithm>
#include <cmath>

namespace futu_trader {

BacktestMetrics Backtester::Run(const std::vector<Tick>& ticks, const ISignalModel& model) const {
  BacktestMetrics m;
  if (ticks.size() < 2) {
    return m;
  }
  double equity = 1.0;
  double peak = 1.0;
  std::size_t wins = 0;
  for (std::size_t i = 1; i < ticks.size(); ++i) {
    const double ret = static_cast<double>(ticks[i].price_minor - ticks[i - 1].price_minor) /
                       static_cast<double>(ticks[i - 1].price_minor);
    const Signal s = model.Predict({ret});
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
      m.max_drawdown = std::max(m.max_drawdown, (peak - equity) / peak);
    }
  }
  m.total_return = equity - 1.0;
  m.annualized_return = m.total_return;
  m.annualized_volatility = std::fabs(m.total_return);
  m.sharpe = (m.annualized_volatility == 0.0) ? 0.0 : m.annualized_return / m.annualized_volatility;
  m.win_rate = static_cast<double>(wins) / static_cast<double>(ticks.size() - 1);
  return m;
}

}  // namespace futu_trader
