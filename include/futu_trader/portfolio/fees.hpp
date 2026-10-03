#pragma once

#include <cstdint>

#include "futu_trader/core/types.hpp"

namespace futu_trader::portfolio {

/**
 * HK equity trading cost schedule. Rates are in parts per billion of turnover so all arithmetic
 * is integer. The defaults are the statutory/exchange charges as best known at the time of
 * writing; they are DATA, change over time, and must be checked against the current HKEX/SFC/
 * broker schedules before live use. Broker commission and platform fee default to 0 because they
 * are account specific: set them from your broker's schedule.
 */
struct HkFeeSchedule {
  const char* asOf{"2024-01 (verify before live)"};
  std::int64_t stampDutyPpb{1'000'000};  // 0.1000%, both sides, rounded UP to whole HKD
  std::int64_t sfcLevyPpb{27'000};       // 0.0027%
  std::int64_t tradingFeePpb{56'500};    // 0.00565%
  std::int64_t frcLevyPpb{1'500};        // 0.00015%
  std::int64_t settlementPpb{20'000};    // 0.002%
  Money settlementMinMills{2'000};       // HKD 2
  Money settlementMaxMills{100'000};     // HKD 100
  std::int64_t commissionPpb{0};         // broker specific
  Money commissionMinMills{0};
  Money platformFeeMills{0};  // per order, broker specific
};

/** Total fee for one fill (or one order's turnover) in mills; 0 for non-positive turnover. */
Money hkFee(const HkFeeSchedule& schedule, Money turnoverMills);

}  // namespace futu_trader::portfolio
