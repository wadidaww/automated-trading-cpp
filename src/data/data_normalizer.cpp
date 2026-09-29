#include "futu_trader/data/data_normalizer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace futu_trader {

auto DataNormalizer::rsi(const std::vector<double>& close, std::size_t period) -> double {
  if (close.size() <= period || period == 0) {
    return 50.0;
  }
  // Wilder's smoothing: seed with the simple average of the first `period` changes, then
  // apply avg = (avg * (period - 1) + current) / period.
  const auto n = static_cast<double>(period);
  double avgGain = 0.0;
  double avgLoss = 0.0;
  for (std::size_t i = 1; i <= period; ++i) {
    const double delta = close[i] - close[i - 1];
    avgGain += std::max(delta, 0.0);
    avgLoss += std::max(-delta, 0.0);
  }
  avgGain /= n;
  avgLoss /= n;
  for (std::size_t i = period + 1; i < close.size(); ++i) {
    const double delta = close[i] - close[i - 1];
    avgGain = (avgGain * (n - 1.0) + std::max(delta, 0.0)) / n;
    avgLoss = (avgLoss * (n - 1.0) + std::max(-delta, 0.0)) / n;
  }
  if (avgLoss == 0.0) {
    return avgGain == 0.0 ? 50.0 : 100.0;
  }
  const double rs = avgGain / avgLoss;
  return 100.0 - (100.0 / (1.0 + rs));
}

auto DataNormalizer::macd(const std::vector<double>& close, std::size_t fast, std::size_t slow,
                          std::size_t signalPeriod) -> std::vector<double> {
  if (close.empty() || fast == 0 || slow == 0 || signalPeriod == 0) {
    return {0.0, 0.0};
  }
  const auto step = [](double value, double x, std::size_t period) {
    const double alpha = 2.0 / (static_cast<double>(period) + 1.0);
    return alpha * x + (1.0 - alpha) * value;
  };
  double fastEma = close.front();
  double slowEma = close.front();
  double macdLine = 0.0;
  double signalLine = 0.0;
  for (std::size_t i = 1; i < close.size(); ++i) {
    fastEma = step(fastEma, close[i], fast);
    slowEma = step(slowEma, close[i], slow);
    macdLine = fastEma - slowEma;
    // The signal line is an EMA of the MACD line itself (seeded at 0, where the series starts).
    signalLine = step(signalLine, macdLine, signalPeriod);
  }
  return {macdLine, signalLine};
}

auto DataNormalizer::bollinger(const std::vector<double>& close, std::size_t period,
                               double stddevMult) -> std::pair<double, double> {
  if (close.size() < period || period == 0) {
    return {0.0, 0.0};
  }
  const auto begin = close.end() - static_cast<std::ptrdiff_t>(period);
  const double mean = std::accumulate(begin, close.end(), 0.0) / static_cast<double>(period);
  double sq = 0.0;
  for (auto it = begin; it != close.end(); ++it) {
    const double d = *it - mean;
    sq += d * d;
  }
  const double stddev = std::sqrt(sq / static_cast<double>(period));
  return {mean - (stddevMult * stddev), mean + (stddevMult * stddev)};
}

double DataNormalizer::vwap(const std::vector<double>& price, const std::vector<double>& volume) {
  if (price.size() != volume.size() || price.empty()) {
    return 0.0;
  }
  double pv = 0.0;
  double vol = 0.0;
  for (std::size_t i = 0; i < price.size(); ++i) {
    pv += price[i] * volume[i];
    vol += volume[i];
  }
  return vol == 0.0 ? 0.0 : pv / vol;
}

double DataNormalizer::obv(const std::vector<double>& close, const std::vector<double>& volume) {
  if (close.size() != volume.size() || close.empty()) {
    return 0.0;
  }
  double obv = 0.0;
  for (std::size_t i = 1; i < close.size(); ++i) {
    if (close[i] > close[i - 1]) {
      obv += volume[i];
    } else if (close[i] < close[i - 1]) {
      obv -= volume[i];
    }
  }
  return obv;
}

auto DataNormalizer::rollingZScore(const std::vector<double>& values) -> double {
  if (values.size() < 2) {
    return 0.0;
  }
  const double mean =
      std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
  double var = 0.0;
  for (double v : values) {
    const double d = v - mean;
    var += d * d;
  }
  const double stddev = std::sqrt(var / static_cast<double>(values.size()));
  return stddev == 0.0 ? 0.0 : (values.back() - mean) / stddev;
}

}  // namespace futu_trader
