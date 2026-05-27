#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace futu_trader {

/** Computes technical indicators used for model features. */
class DataNormalizer {
 public:
  static double rsi(const std::vector<double>& close, std::size_t period);
  static std::vector<double> macd(const std::vector<double>& close, std::size_t fast,
                                  std::size_t slow);
  static std::pair<double, double> bollinger(const std::vector<double>& close, std::size_t period,
                                             double stddevMult);
  static double vwap(const std::vector<double>& price, const std::vector<double>& volume);
  static double obv(const std::vector<double>& close, const std::vector<double>& volume);
  static double rollingZScore(const std::vector<double>& values);
};

}  // namespace futu_trader
