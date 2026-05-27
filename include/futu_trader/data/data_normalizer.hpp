#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace futu_trader {

/** Computes technical indicators used for model features. */
class DataNormalizer {
 public:
  static double RSI(const std::vector<double>& close, std::size_t period);
  static std::vector<double> MACD(const std::vector<double>& close, std::size_t fast,
                                  std::size_t slow);
  static std::pair<double, double> Bollinger(const std::vector<double>& close, std::size_t period,
                                             double stddev_mult);
  static double VWAP(const std::vector<double>& price, const std::vector<double>& volume);
  static double OBV(const std::vector<double>& close, const std::vector<double>& volume);
  static double RollingZScore(const std::vector<double>& values);
};

}  // namespace futu_trader
