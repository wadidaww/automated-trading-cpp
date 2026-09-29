#pragma once

#include <cstdint>

#include "futu_trader/core/types.hpp"

namespace futu_trader::sizing {

/**
 * Position sizing. Inputs that are NaN, negative or otherwise nonsensical yield 0 (no trade),
 * never a large number. Results are whole board lots, rounded DOWN, and never exceed the caps.
 * Fractions and volatilities are doubles (they are statistical estimates); money and quantity
 * are integers.
 */
struct SizingLimits {
  std::int64_t lotSize{0};         // board lot; 0 yields no size
  Money maxOrderNotionalMills{0};  // hard cap; 0 yields no size
  double maxEquityFraction{0.0};   // cap on notional / equity, e.g. 0.10; 0 yields no size
};

/**
 * Fractional Kelly: notional = equity * min(kellyMultiplier * kelly(win, payoff), cap).
 * `kellyMultiplier` in (0,1] shrinks the (over-optimistic, estimation-noise-prone) full Kelly
 * bet; 0.25 is a common starting point.
 */
std::int64_t kellyQuantity(Money equityMills, Money priceMills, double winRate, double winLossRatio,
                           double kellyMultiplier, const SizingLimits& limits);

/**
 * Volatility targeting: notional = equity * targetVol / instrumentVol, both as daily fractions
 * (0.01 == 1%), capped by `limits`. Sizes down when the instrument is volatile.
 */
std::int64_t volTargetQuantity(Money equityMills, Money priceMills, double targetDailyVol,
                               double instrumentDailyVol, const SizingLimits& limits);

}  // namespace futu_trader::sizing
