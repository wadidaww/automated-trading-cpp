#pragma once

#include <string>
#include <unordered_map>

#include "futu_trader/core/types.hpp"

namespace futu_trader {

struct RiskConfig {
  Money maxPositionNotionalMinor{0};
  Money maxPortfolioNotionalMinor{0};
  Money maxDailyLossMinor{0};
  std::size_t maxOpenOrders{0};
  double concentrationLimit{1.0};
};

class KellyCriterion {
 public:
  static double fraction(double winRate, double win_loss_ratio);
};

class DrawdownMonitor {
 public:
  void observe(double equity);
  double maxDrawdown() const;

 private:
  double peak_{0.0};
  double maxDrawdown_{0.0};
};

class RiskEngine {
 public:
  explicit RiskEngine(RiskConfig config);
  bool canPlace(const Order& order, const std::unordered_map<std::string, Money>& positions,
                Money portfolioNotional, Money dailyPnl, std::size_t openOrders) const;

 private:
  RiskConfig config_;
};

}  // namespace futu_trader
