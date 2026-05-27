#include <cassert>
#include <cmath>
#include <vector>

#include "futu_trader/data/data_normalizer.hpp"

int main() {
  const std::vector<double> close{100, 101, 103, 102, 104, 106};
  const auto rsi = futu_trader::DataNormalizer::RSI(close, 3);
  assert(rsi >= 0.0 && rsi <= 100.0);

  const auto macd = futu_trader::DataNormalizer::MACD(close, 3, 5);
  assert(macd.size() == 2);

  const auto [low, high] = futu_trader::DataNormalizer::Bollinger(close, 5, 2.0);
  assert(low <= high);

  const auto vwap = futu_trader::DataNormalizer::VWAP({1, 2, 3}, {10, 10, 10});
  assert(std::fabs(vwap - 2.0) < 1e-9);

  return 0;
}
