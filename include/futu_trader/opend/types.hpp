#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/core/types.hpp"

namespace futu_trader::opend {

/**
 * Futu quotes prices, quantities and cash as doubles. We store them as int64 "mills" (1/1000 of
 * the currency unit; HK prices have at most 3 decimals). This scale is a placeholder until the
 * per-instrument table (tick, lot, scale) lands.
 */
inline constexpr std::int64_t kMoneyScale = 1000;

/** Converts a double to int64 mills, rejecting NaN/inf/out-of-range instead of wrapping. */
inline Result<Money> toMinor(double value) {
  if (!std::isfinite(value)) {
    return Error{ErrorCode::kInvalidArg, "non-finite amount"};
  }
  const double scaled = std::round(value * static_cast<double>(kMoneyScale));
  constexpr double kLimit = 9.0e18;  // just under INT64_MAX
  if (scaled > kLimit || scaled < -kLimit) {
    return Error{ErrorCode::kInvalidArg, "amount out of range"};
  }
  return static_cast<Money>(scaled);
}

/** Integral share quantity from a double; fractional quantities are rejected. */
inline Result<std::int64_t> toQuantity(double value) {
  if (!std::isfinite(value) || std::fabs(value) > 9.0e15) {
    return Error{ErrorCode::kInvalidArg, "invalid quantity"};
  }
  const double rounded = std::round(value);
  if (std::fabs(value - rounded) > 1e-6) {
    return Error{ErrorCode::kInvalidArg, "fractional quantity not supported"};
  }
  return static_cast<std::int64_t>(rounded);
}

enum class TrdMarket : std::uint8_t { kHK = 1, kUS = 2, kCN = 3 };

// Qot_Common.QotMarket values used by Futu security identifiers.
inline constexpr std::int32_t kQotMarketHkSecurity = 1;
inline constexpr std::int32_t kQotMarketUsSecurity = 11;

struct SecurityRef {
  std::int32_t market{kQotMarketHkSecurity};
  std::string code;  // e.g. "00700"
};

/** Parses "HK.00700" / "US.AAPL". */
Result<SecurityRef> parseSecurity(const std::string& text);

struct TrdAccount {
  TrdEnv env{TrdEnv::kSimulate};
  std::uint64_t accId{0};
  std::vector<std::int32_t> markets;  // Trd_Common.TrdMarket authorisations
};

struct AccountHeader {
  TrdEnv env{TrdEnv::kSimulate};
  std::uint64_t accId{0};
  TrdMarket market{TrdMarket::kHK};
};

struct FundsInfo {
  Money power{0};
  Money totalAssets{0};
  Money cash{0};
  Money marketValue{0};
  Money frozenCash{0};
};

struct PositionInfo {
  std::string code;
  std::int64_t qty{0};
  std::int64_t canSellQty{0};
  Money costPrice{0};
  Money price{0};
};

struct BasicQuote {
  SecurityRef security;
  Money curPrice{0};
  Money lastClose{0};
  std::int64_t volume{0};
  bool suspended{false};
  double updateTimestamp{0.0};  // seconds since epoch as reported by OpenD
};

struct Bar {
  std::string time;  // "YYYY-MM-DD HH:MM:SS" exchange local time as reported by OpenD
  Money open{0};
  Money high{0};
  Money low{0};
  Money close{0};
  std::int64_t volume{0};
};

/** Qot_Common.SubType values. */
enum class SubType : std::uint8_t {
  kBasic = 1,
  kOrderBook = 2,
  kTicker = 4,
  kKlDay = 6,
  kKl1Min = 11,
};

/** Qot_Common.KLType values. */
enum class KlType : std::uint8_t {
  k1Min = 1,
  kDay = 2,
  k5Min = 6,
  k15Min = 7,
  k30Min = 8,
  k60Min = 9
};

}  // namespace futu_trader::opend
