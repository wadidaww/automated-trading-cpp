#include "futu_trader/risk/risk_engine.hpp"

#include <algorithm>

namespace futu_trader {

double KellyCriterion::Fraction(double win_rate, double win_loss_ratio) {
  if (win_loss_ratio <= 0.0) {
    return 0.0;
  }
  return std::max(0.0, win_rate - ((1.0 - win_rate) / win_loss_ratio));
}

void DrawdownMonitor::Observe(double equity) {
  peak_ = std::max(peak_, equity);
  if (peak_ > 0.0) {
    max_drawdown_ = std::max(max_drawdown_, (peak_ - equity) / peak_);
  }
}

double DrawdownMonitor::MaxDrawdown() const { return max_drawdown_; }

RiskEngine::RiskEngine(RiskConfig config) : config_(config) {}

bool RiskEngine::CanPlace(const Order& order, const std::unordered_map<std::string, Money>& positions,
                          Money portfolio_notional, Money daily_pnl,
                          std::size_t open_orders) const {
  if (open_orders >= config_.max_open_orders) {
    return false;
  }
  if (-daily_pnl > config_.max_daily_loss_minor) {
    return false;
  }

  const Money order_notional = order.limit_price_minor * order.quantity;
  const auto it = positions.find(order.symbol);
  const Money symbol_notional = (it != positions.end() ? it->second : 0) + order_notional;
  if (symbol_notional > config_.max_position_notional_minor) {
    return false;
  }
  if (portfolio_notional + order_notional > config_.max_portfolio_notional_minor) {
    return false;
  }
  if (config_.max_portfolio_notional_minor > 0 &&
      static_cast<double>(symbol_notional) /
              static_cast<double>(config_.max_portfolio_notional_minor) >
          config_.concentration_limit) {
    return false;
  }
  return true;
}

}  // namespace futu_trader
