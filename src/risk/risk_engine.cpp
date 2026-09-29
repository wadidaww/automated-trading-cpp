#include "futu_trader/risk/risk_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace futu_trader {

namespace {

__extension__ using Int128 = __int128;  // GCC/Clang extension; used for overflow-free compares

bool checkedMul(Money a, Money b, Money& out) { return !__builtin_mul_overflow(a, b, &out); }
bool checkedAdd(Money a, Money b, Money& out) { return !__builtin_add_overflow(a, b, &out); }

// std::abs(INT64_MIN) is undefined, so treat it as overflow.
bool checkedAbs(Money a, Money& out) {
  if (a == INT64_MIN) {
    return false;
  }
  out = std::abs(a);
  return true;
}

}  // namespace

const char* toString(RiskReject reason) {
  switch (reason) {
    case RiskReject::kOk:
      return "ok";
    case RiskReject::kInvalidOrder:
      return "invalid_order";
    case RiskReject::kMaxOpenOrders:
      return "max_open_orders";
    case RiskReject::kDailyLoss:
      return "daily_loss";
    case RiskReject::kPositionLimit:
      return "position_limit";
    case RiskReject::kPortfolioLimit:
      return "portfolio_limit";
    case RiskReject::kConcentration:
      return "concentration";
    case RiskReject::kOverflow:
      return "overflow";
    case RiskReject::kKillSwitch:
      return "kill_switch";
    case RiskReject::kUnknownInstrument:
      return "unknown_instrument";
    case RiskReject::kLotSize:
      return "lot_size";
    case RiskReject::kTickSize:
      return "tick_size";
    case RiskReject::kPriceBand:
      return "price_band";
    case RiskReject::kStaleQuote:
      return "stale_quote";
    case RiskReject::kMaxOrderNotional:
      return "max_order_notional";
    case RiskReject::kShortSale:
      return "short_sale";
    case RiskReject::kRateLimit:
      return "rate_limit";
    case RiskReject::kUnresolvedOrder:
      return "unresolved_order";
    case RiskReject::kSelfTrade:
      return "self_trade";
  }
  return "unknown";
}

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

RiskReject RiskEngine::check(const Order& order,
                             const std::unordered_map<std::string, Money>& positions,
                             Money portfolioNotional, Money dailyPnl,
                             std::size_t openOrders) const {
  if (order.quantity <= 0 || order.limitPriceMinor <= 0) {
    return RiskReject::kInvalidOrder;
  }

  Money orderNotional = 0;
  if (!checkedMul(order.limitPriceMinor, order.quantity, orderNotional)) {
    return RiskReject::kOverflow;
  }
  Money signedOrder = 0;
  if (!checkedMul(orderNotional, sideSign(order.side), signedOrder)) {
    return RiskReject::kOverflow;
  }

  const auto it = positions.find(order.symbol);
  const Money current = it != positions.end() ? it->second : 0;
  Money after = 0;
  if (!checkedAdd(current, signedOrder, after)) {
    return RiskReject::kOverflow;
  }
  Money absCurrent = 0;
  Money absAfter = 0;
  if (!checkedAbs(current, absCurrent) || !checkedAbs(after, absAfter)) {
    return RiskReject::kOverflow;
  }

  // Exposure-reducing orders skip every limit below, including the daily-loss and open-order
  // caps: a breach must never prevent flattening.
  if (absAfter < absCurrent) {
    return RiskReject::kOk;
  }
  if (openOrders >= config_.maxOpenOrders) {
    return RiskReject::kMaxOpenOrders;
  }
  // Compare without negating dailyPnl: -INT64_MIN overflows. An unset loss limit rejects.
  if (config_.maxDailyLossMinor <= 0 || dailyPnl < -config_.maxDailyLossMinor) {
    return RiskReject::kDailyLoss;
  }
  if (absAfter > config_.maxPositionNotionalMinor) {
    return RiskReject::kPositionLimit;
  }
  Money portfolioAfter = 0;
  if (!checkedAdd(portfolioNotional, absAfter - absCurrent, portfolioAfter)) {
    return RiskReject::kOverflow;
  }
  if (portfolioAfter > config_.maxPortfolioNotionalMinor) {
    return RiskReject::kPortfolioLimit;
  }
  if (config_.concentrationLimit <= 0.0) {
    return RiskReject::kConcentration;  // unset concentration limit fails closed
  }
  if (config_.maxPortfolioNotionalMinor > 0) {
    // Exact integer comparison in basis points: absAfter / maxPortfolio > limit. Comparing
    // doubles rejects orders sitting exactly on the limit (0.3 is not representable).
    const auto limitBps = static_cast<Int128>(std::llround(config_.concentrationLimit * 10000.0));
    if (static_cast<Int128>(absAfter) * 10000 >
        limitBps * static_cast<Int128>(config_.maxPortfolioNotionalMinor)) {
      return RiskReject::kConcentration;
    }
  }
  return RiskReject::kOk;
}

}  // namespace futu_trader
