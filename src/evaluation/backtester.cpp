#include "futu_trader/evaluation/backtester.hpp"

#include <algorithm>
#include <cmath>

namespace futu_trader {

namespace {

int positionFor(SignalAction action) {
  switch (action) {
    case SignalAction::kBuy:
      return 1;
    case SignalAction::kSell:
      return -1;
    case SignalAction::kHold:
      break;
  }
  return 0;
}

}  // namespace

BacktestMetrics Backtester::run(const std::vector<Tick>& ticks, const ISignalModel& model,
                                BacktestConfig config) const {
  BacktestMetrics m;
  if (ticks.size() < 3) {
    return m;
  }
  const double feeRate = config.feeBps / 10000.0;

  double equity = 1.0;
  double peak = 1.0;
  int position = 0;      // position held during the current bar: -1, 0, +1
  int nextPosition = 0;  // position decided from the previous bar's return
  std::size_t inMarket = 0;
  std::size_t wins = 0;
  std::vector<double> stepReturns;
  stepReturns.reserve(ticks.size());

  for (std::size_t i = 1; i < ticks.size(); ++i) {
    if (ticks[i - 1].priceMinor <= 0) {
      continue;
    }
    const double ret = static_cast<double>(ticks[i].priceMinor - ticks[i - 1].priceMinor) /
                       static_cast<double>(ticks[i - 1].priceMinor);

    // Trade into the position decided at the end of the previous bar, then earn this bar's return.
    const int change = std::abs(nextPosition - position);
    position = nextPosition;
    if (change > 0) {
      ++m.trades;
    }
    const double pnl = (static_cast<double>(position) * ret) - (feeRate * change);
    if (position != 0) {
      ++inMarket;
      if (pnl > 0.0) {
        ++wins;
      }
    }
    stepReturns.push_back(pnl);
    equity *= (1.0 + pnl);
    peak = std::max(peak, equity);
    m.maxDrawdown = std::max(m.maxDrawdown, (peak - equity) / peak);

    // Decide the next bar's position using only information available now.
    const Signal s = model.predict({ret});
    nextPosition = positionFor(s.action);
  }

  const auto n = static_cast<double>(stepReturns.size());
  if (n < 2.0) {
    return m;
  }
  double mean = 0.0;
  for (double r : stepReturns) {
    mean += r;
  }
  mean /= n;
  double var = 0.0;
  for (double r : stepReturns) {
    var += (r - mean) * (r - mean);
  }
  const double stddev = std::sqrt(var / (n - 1.0));

  m.totalReturn = equity - 1.0;
  m.annualizedReturn = equity > 0.0 ? std::pow(equity, config.periodsPerYear / n) - 1.0 : -1.0;
  m.annualizedVolatility = stddev * std::sqrt(config.periodsPerYear);
  m.sharpe = stddev == 0.0 ? 0.0 : (mean / stddev) * std::sqrt(config.periodsPerYear);
  m.winRate = inMarket == 0 ? 0.0 : static_cast<double>(wins) / static_cast<double>(inMarket);
  return m;
}

}  // namespace futu_trader
