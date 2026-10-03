#include "futu_trader/portfolio/fees.hpp"

#include <algorithm>

namespace futu_trader::portfolio {

namespace {

__extension__ using Int128 = __int128;
constexpr std::int64_t kBillion = 1'000'000'000;
constexpr Money kCentMills = 10;  // HKD 0.01
constexpr Money kDollarMills = 1000;

// turnover * ppb / 1e9, rounded half-up to a multiple of `unit` mills.
Money roundedCharge(Money turnover, std::int64_t ppb, Money unit) {
  const Int128 numerator = static_cast<Int128>(turnover) * ppb;
  const Int128 scaledUnit = static_cast<Int128>(unit) * kBillion;
  return static_cast<Money>(((numerator + (scaledUnit / 2)) / scaledUnit) * unit);
}

// Same, but rounded UP (stamp duty is rounded up to the next whole dollar).
Money ceilCharge(Money turnover, std::int64_t ppb, Money unit) {
  const Int128 numerator = static_cast<Int128>(turnover) * ppb;
  const Int128 scaledUnit = static_cast<Int128>(unit) * kBillion;
  return static_cast<Money>(((numerator + scaledUnit - 1) / scaledUnit) * unit);
}

}  // namespace

Money hkFee(const HkFeeSchedule& s, Money turnoverMills) {
  if (turnoverMills <= 0) {
    return 0;
  }
  Money total = ceilCharge(turnoverMills, s.stampDutyPpb, kDollarMills);
  total += roundedCharge(turnoverMills, s.sfcLevyPpb, kCentMills);
  total += roundedCharge(turnoverMills, s.tradingFeePpb, kCentMills);
  total += roundedCharge(turnoverMills, s.frcLevyPpb, kCentMills);
  total += std::clamp(roundedCharge(turnoverMills, s.settlementPpb, kCentMills),
                      s.settlementMinMills, s.settlementMaxMills);
  total +=
      std::max(roundedCharge(turnoverMills, s.commissionPpb, kCentMills), s.commissionMinMills);
  total += s.platformFeeMills;
  return total;
}

}  // namespace futu_trader::portfolio
