#include "futu_trader/risk/risk_engine.hpp"

#include <algorithm>

namespace futu_trader {

double KellyCriterion::fraction(double winRate, double win_loss_ratio) {
  if (win_loss_ratio <= 0.0) {
    return 0.0;
  }
  return std::max(0.0, winRate - ((1.0 - winRate) / win_loss_ratio));
}

void DrawdownMonitor::observe(double equity) {
  peak_ = std::max(peak_, equity);
  if (peak_ > 0.0) {
    maxDrawdown_ = std::max(maxDrawdown_, (peak_ - equity) / peak_);
  }
}

double DrawdownMonitor::maxDrawdown() const { return maxDrawdown_; }

RiskEngine::RiskEngine(RiskConfig config) : config_(config) {}

bool RiskEngine::canPlace(const Order& order, const std::unordered_map<std::string, Money>& positions,
                          Money portfolioNotional, Money dailyPnl,
                          std::size_t openOrders) const {
  if (openOrders >= config_.maxOpenOrders) {
    return false;
  }
  if (-dailyPnl > config_.maxDailyLossMinor) {
    return false;
  }

  const Money orderNotional = order.limitPriceMinor * order.quantity;
  const auto it = positions.find(order.symbol);
  const Money symbolNotional = (it != positions.end() ? it->second : 0) + orderNotional;
  if (symbolNotional > config_.maxPositionNotionalMinor) {
    return false;
  }
  if (portfolioNotional + orderNotional > config_.maxPortfolioNotionalMinor) {
    return false;
  }
  if (config_.maxPortfolioNotionalMinor > 0 &&
      static_cast<double>(symbolNotional) /
              static_cast<double>(config_.maxPortfolioNotionalMinor) >
          config_.concentrationLimit) {
    return false;
  }
  return true;
}

}  // namespace futu_trader
