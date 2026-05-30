#pragma once

#include <cstddef>
#include <utility>
#include <vector>

namespace futu_trader {

/** Computes technical indicators used for model features. */
class DataNormalizer {
 public:
  static auto rsi(const std::vector<double>& close, std::size_t period) -> double;
  static auto macd(const std::vector<double>& close, std::size_t fast,
                   std::size_t slow) -> std::vector<double>;
  static auto bollinger(const std::vector<double>& close, std::size_t period,
                        double stddevMult) -> std::pair<double, double>;
  static double vwap(const std::vector<double>& price, const std::vector<double>& volume);
  static double obv(const std::vector<double>& close, const std::vector<double>& volume);
  static double rollingZScore(const std::vector<double>& values);
};

}  // namespace futu_trader
