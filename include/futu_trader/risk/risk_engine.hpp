#pragma once

#include <cstddef>
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

/** Why a pre-trade check rejected an order. kOk means the order may be placed. */
enum class RiskReject : uint8_t {
  kOk,
  kInvalidOrder,
  kMaxOpenOrders,
  kDailyLoss,
  kPositionLimit,
  kPortfolioLimit,
  kConcentration,
  kOverflow,
};

const char* toString(RiskReject reason);

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

/**
 * Stateless pre-trade risk check. Every limit defaults to zero, so a default-constructed
 * RiskConfig rejects everything (fail closed).
 *
 * `positions` holds signed per-symbol notional (long > 0, short < 0). `portfolioNotional` is the
 * current gross notional. Orders that reduce exposure are checked only for validity, open-order
 * and daily-loss limits.
 */
class RiskEngine {
 public:
  explicit RiskEngine(RiskConfig config);

  RiskReject check(const Order& order, const std::unordered_map<std::string, Money>& positions,
                   Money portfolioNotional, Money dailyPnl, std::size_t openOrders) const;

  bool canPlace(const Order& order, const std::unordered_map<std::string, Money>& positions,
                Money portfolioNotional, Money dailyPnl, std::size_t openOrders) const {
    return check(order, positions, portfolioNotional, dailyPnl, openOrders) == RiskReject::kOk;
  }

 private:
  RiskConfig config_;
};

}  // namespace futu_trader
