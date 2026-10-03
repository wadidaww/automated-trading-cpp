#include "futu_trader/data/data_normalizer.hpp"

#include <gtest/gtest.h>

#include <vector>

using futu_trader::DataNormalizer;

TEST(DataNormalizer, RsiMatchesWilderReference) {
  // Classic Wilder RSI worked example (period 14) from Wilder/StockCharts, expected ~70.53.
  const std::vector<double> close{44.3389, 44.0902, 44.1497, 43.6124, 44.3278,
                                  44.8264, 45.0955, 45.4245, 45.8433, 46.0826,
                                  45.8931, 46.0328, 45.6140, 46.2820, 46.2820};
  EXPECT_NEAR(DataNormalizer::rsi(close, 14), 70.53, 0.1);
}

TEST(DataNormalizer, RsiEdgeCases) {
  EXPECT_DOUBLE_EQ(DataNormalizer::rsi({1, 2}, 14), 50.0);        // not enough data
  EXPECT_DOUBLE_EQ(DataNormalizer::rsi({1, 2, 3, 4}, 3), 100.0);  // only gains
  EXPECT_DOUBLE_EQ(DataNormalizer::rsi({5, 5, 5, 5}, 3), 50.0);   // flat
  EXPECT_DOUBLE_EQ(DataNormalizer::rsi({4, 3, 2, 1}, 3), 0.0);    // only losses
}

TEST(DataNormalizer, MacdOfConstantSeriesIsZero) {
  const auto m = DataNormalizer::macd({10, 10, 10, 10, 10}, 3, 5);
  ASSERT_EQ(m.size(), 2U);
  EXPECT_DOUBLE_EQ(m[0], 0.0);
  EXPECT_DOUBLE_EQ(m[1], 0.0);
}

TEST(DataNormalizer, MacdSignalIsEmaOfMacdNotAFixedFraction) {
  // On a rising series the fast EMA leads the slow EMA, so MACD > 0, and the signal line (an EMA
  // of MACD) lags it: 0 < signal < macd. The previous implementation used signal = 0.8 * macd.
  std::vector<double> rising;
  for (int i = 0; i < 40; ++i) {
    rising.push_back(100.0 + i);
  }
  const auto m = DataNormalizer::macd(rising, 12, 26);
  EXPECT_GT(m[0], 0.0);
  EXPECT_GT(m[1], 0.0);
  EXPECT_LT(m[1], m[0]);
  EXPECT_NE(m[1], m[0] * 0.8);
}

TEST(DataNormalizer, BollingerKnownValues) {
  // mean 3, population stddev sqrt(2): bands are 3 -/+ 2*sqrt(2).
  const auto [low, high] = DataNormalizer::bollinger({1, 2, 3, 4, 5}, 5, 2.0);
  EXPECT_NEAR(low, 3.0 - 2.0 * 1.4142135623730951, 1e-12);
  EXPECT_NEAR(high, 3.0 + 2.0 * 1.4142135623730951, 1e-12);
}

TEST(DataNormalizer, VwapAndObv) {
  EXPECT_NEAR(DataNormalizer::vwap({1, 2, 3}, {10, 10, 10}), 2.0, 1e-12);
  EXPECT_DOUBLE_EQ(DataNormalizer::vwap({1, 2}, {0, 0}), 0.0);
  EXPECT_DOUBLE_EQ(DataNormalizer::obv({1, 2, 1, 1}, {5, 10, 4, 7}), 10.0 - 4.0);
}

TEST(DataNormalizer, RollingZScore) {
  EXPECT_DOUBLE_EQ(DataNormalizer::rollingZScore({5}), 0.0);
  EXPECT_DOUBLE_EQ(DataNormalizer::rollingZScore({2, 2, 2}), 0.0);
  // values {0,0,0,4}: mean 1, population stddev sqrt(3), z of last = 3/sqrt(3).
  EXPECT_NEAR(DataNormalizer::rollingZScore({0, 0, 0, 4}), 1.7320508075688772, 1e-12);
}
