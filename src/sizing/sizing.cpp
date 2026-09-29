#include "futu_trader/sizing/sizing.hpp"

#include <algorithm>
#include <cmath>

#include "futu_trader/risk/risk_engine.hpp"

namespace futu_trader::sizing {

namespace {

bool usable(double value) { return std::isfinite(value) && value > 0.0; }

// Whole lots that fit into `notional`, after applying every cap. Doubles only carry the sizing
// fraction; the final rounding is integer.
std::int64_t lotsFor(double notionalMills, Money equityMills, Money priceMills,
                     const SizingLimits& limits) {
  if (!usable(notionalMills) || equityMills <= 0 || priceMills <= 0 || limits.lotSize <= 0 ||
      limits.maxOrderNotionalMills <= 0 || !usable(limits.maxEquityFraction)) {
    return 0;
  }
  const double cap = std::min({notionalMills, static_cast<double>(limits.maxOrderNotionalMills),
                               static_cast<double>(equityMills) * limits.maxEquityFraction});
  const double lotValue = static_cast<double>(priceMills) * static_cast<double>(limits.lotSize);
  if (!std::isfinite(cap) || !std::isfinite(lotValue) || lotValue <= 0.0) {
    return 0;
  }
  const double lots = std::floor(cap / lotValue);
  if (!(lots >= 1.0) || lots > 1e12) {
    return 0;
  }
  return static_cast<std::int64_t>(lots) * limits.lotSize;
}

}  // namespace

std::int64_t kellyQuantity(Money equityMills, Money priceMills, double winRate, double winLossRatio,
                           double kellyMultiplier, const SizingLimits& limits) {
  if (!std::isfinite(winRate) || winRate <= 0.0 || winRate >= 1.0 || !usable(winLossRatio) ||
      !usable(kellyMultiplier) || kellyMultiplier > 1.0) {
    return 0;
  }
  const double fraction = KellyCriterion::fraction(winRate, winLossRatio) * kellyMultiplier;
  return lotsFor(static_cast<double>(equityMills) * fraction, equityMills, priceMills, limits);
}

std::int64_t volTargetQuantity(Money equityMills, Money priceMills, double targetDailyVol,
                               double instrumentDailyVol, const SizingLimits& limits) {
  if (!usable(targetDailyVol) || !usable(instrumentDailyVol)) {
    return 0;
  }
  const double notional = static_cast<double>(equityMills) * (targetDailyVol / instrumentDailyVol);
  return lotsFor(notional, equityMills, priceMills, limits);
}

}  // namespace futu_trader::sizing
