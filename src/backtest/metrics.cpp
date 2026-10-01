#include "futu_trader/backtest/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "futu_trader/backtest/synthetic.hpp"

namespace futu_trader::backtest {

std::vector<Money> resampleEquity(const std::vector<EquityPoint>& points, std::int64_t periodNs) {
  std::vector<Money> out;
  if (points.empty() || periodNs <= 0) {
    return out;
  }
  const std::int64_t origin = points.front().tsNs;
  std::int64_t currentBucket = 0;
  Money last = points.front().equityMills;
  for (const auto& point : points) {
    const std::int64_t bucket = (point.tsNs - origin) / periodNs;
    if (bucket != currentBucket) {
      out.push_back(last);  // close the bucket we were in; empty buckets are skipped
      currentBucket = bucket;
    }
    last = point.equityMills;
  }
  out.push_back(last);
  return out;
}

std::vector<double> returnsFromEquity(const std::vector<Money>& equity) {
  std::vector<double> returns;
  if (equity.size() < 2) {
    return returns;
  }
  returns.reserve(equity.size() - 1);
  for (std::size_t i = 1; i < equity.size(); ++i) {
    if (equity[i - 1] <= 0 || equity[i] <= 0) {
      return {};
    }
    returns.push_back((static_cast<double>(equity[i]) / static_cast<double>(equity[i - 1])) - 1.0);
  }
  return returns;
}

double sharpeOf(const std::vector<double>& returns, double periodsPerYear) {
  if (returns.size() < 2) {
    return 0.0;
  }
  const auto n = static_cast<double>(returns.size());
  const double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / n;
  double var = 0.0;
  for (const double r : returns) {
    var += (r - mean) * (r - mean);
  }
  const double sd = std::sqrt(var / (n - 1.0));
  return sd == 0.0 ? 0.0 : (mean / sd) * std::sqrt(periodsPerYear);
}

PerformanceMetrics computeMetrics(const std::vector<Money>& equity, double periodsPerYear) {
  PerformanceMetrics m;
  if (equity.size() < 2 || !(periodsPerYear > 0.0)) {
    return m;
  }
  const auto returns = returnsFromEquity(equity);
  if (returns.empty()) {
    m.ruined = true;
    m.totalReturn = equity.back() <= 0 ? -1.0 : 0.0;
    return m;
  }
  const auto n = static_cast<double>(returns.size());
  m.periods = returns.size();
  m.totalReturn = (static_cast<double>(equity.back()) / static_cast<double>(equity.front())) - 1.0;
  m.annualizedReturn = std::pow(1.0 + m.totalReturn, periodsPerYear / n) - 1.0;

  const double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / n;
  double var = 0.0;
  double downside = 0.0;
  for (const double r : returns) {
    var += (r - mean) * (r - mean);
    downside += std::min(r, 0.0) * std::min(r, 0.0);
  }
  const double sd = returns.size() > 1 ? std::sqrt(var / (n - 1.0)) : 0.0;
  const double downsideDev = std::sqrt(downside / n);
  m.annualizedVol = sd * std::sqrt(periodsPerYear);
  m.sharpe = sd == 0.0 ? 0.0 : (mean / sd) * std::sqrt(periodsPerYear);
  m.sortino = downsideDev == 0.0 ? 0.0 : (mean / downsideDev) * std::sqrt(periodsPerYear);

  Money peak = equity.front();
  std::size_t peakIdx = 0;
  for (std::size_t i = 0; i < equity.size(); ++i) {
    if (equity[i] >= peak) {
      peak = equity[i];
      peakIdx = i;
    } else {
      m.maxDrawdown = std::max(m.maxDrawdown,
                               static_cast<double>(peak - equity[i]) / static_cast<double>(peak));
      m.maxDrawdownPeriods = std::max(m.maxDrawdownPeriods, i - peakIdx);
    }
  }
  m.calmar = m.maxDrawdown == 0.0 ? 0.0 : m.annualizedReturn / m.maxDrawdown;
  m.ratiosReliable = m.periods >= kMinReliablePeriods;
  return m;
}

TradeStats computeTradeStats(const std::vector<Money>& closedTradePnl) {
  TradeStats stats;
  stats.count = closedTradePnl.size();
  double grossWin = 0.0;
  double grossLoss = 0.0;
  for (const Money pnl : closedTradePnl) {
    if (pnl > 0) {
      ++stats.wins;
      grossWin += static_cast<double>(pnl);
    } else if (pnl < 0) {
      ++stats.losses;
      grossLoss += static_cast<double>(pnl);
    }
  }
  if (stats.count > 0) {
    stats.hitRate = static_cast<double>(stats.wins) / static_cast<double>(stats.count);
  }
  if (stats.wins > 0) {
    stats.avgWinMills = grossWin / static_cast<double>(stats.wins);
  }
  if (stats.losses > 0) {
    stats.avgLossMills = grossLoss / static_cast<double>(stats.losses);
    stats.profitFactor = grossWin / -grossLoss;
  } else {
    stats.profitFactor = std::numeric_limits<double>::quiet_NaN();  // undefined, not "worst"
  }
  return stats;
}

ConfidenceInterval bootstrapSharpeCi(const std::vector<double>& returns, double periodsPerYear,
                                     std::size_t resamples, std::uint64_t seed, double alpha) {
  ConfidenceInterval ci;
  if (returns.size() < 2 || resamples == 0 || (alpha <= 0.0 || alpha >= 1.0)) {
    return ci;
  }
  Rng rng(seed);
  std::vector<double> sharpes;
  sharpes.reserve(resamples);
  std::vector<double> sample(returns.size());
  for (std::size_t r = 0; r < resamples; ++r) {
    for (auto& value : sample) {
      value = returns[rng.below(returns.size())];
    }
    sharpes.push_back(sharpeOf(sample, periodsPerYear));
  }
  std::sort(sharpes.begin(), sharpes.end());
  const auto at = [&](double q) {
    const auto idx = static_cast<std::size_t>(q * static_cast<double>(sharpes.size() - 1));
    return sharpes[idx];
  };
  ci.lo = at(alpha / 2.0);
  ci.hi = at(1.0 - (alpha / 2.0));
  ci.valid = true;
  return ci;
}

}  // namespace futu_trader::backtest
