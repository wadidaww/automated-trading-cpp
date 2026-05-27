#pragma once

#include <string>
#include <unordered_map>

#include "futu_trader/core/types.hpp"

namespace futu_trader {

struct RiskConfig {
  Money max_position_notional_minor{0};
  Money max_portfolio_notional_minor{0};
  Money max_daily_loss_minor{0};
  std::size_t max_open_orders{0};
  double concentration_limit{1.0};
};

class KellyCriterion {
 public:
  static double Fraction(double win_rate, double win_loss_ratio);
};

class DrawdownMonitor {
 public:
  void Observe(double equity);
  double MaxDrawdown() const;

 private:
  double peak_{0.0};
  double max_drawdown_{0.0};
};

class RiskEngine {
 public:
  explicit RiskEngine(RiskConfig config);
  bool CanPlace(const Order& order, const std::unordered_map<std::string, Money>& positions,
                Money portfolio_notional, Money daily_pnl, std::size_t open_orders) const;

 private:
  RiskConfig config_;
};

}  // namespace futu_trader
