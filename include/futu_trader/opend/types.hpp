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

}  // namespace futu_trader::opend

namespace futu_trader::oms {
class TradeTarget;
}

namespace futu_trader::opend {

/**
 * Identifies the account and environment a request is sent to. There is deliberately NO public
 * way to build a REAL header: only oms::TradeTarget can, and a REAL TradeTarget requires a
 * LiveApproval that only LiveGate::approveReal can issue. So "sent to real money" is unforgeable
 * at compile time, not just by convention.
 */
class AccountHeader {
 public:
  AccountHeader() = delete;
  static AccountHeader simulate(std::uint64_t accId, TrdMarket market) {
    return {TrdEnv::kSimulate, accId, market};
  }
  TrdEnv env() const { return env_; }
  std::uint64_t accId() const { return accId_; }
  TrdMarket market() const { return market_; }

 private:
  friend class oms::TradeTarget;
  AccountHeader(TrdEnv env, std::uint64_t accId, TrdMarket market)
      : env_(env), accId_(accId), market_(market) {}
  TrdEnv env_;
  std::uint64_t accId_;
  TrdMarket market_;
};

/**
 * An account header as reported by the broker on a push. It describes where an event happened
 * and cannot be used to address a request.
 */
struct WireHeader {
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
  std::int64_t qty{0};  // signed: long > 0, short < 0
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

struct PlaceOrderRequest {
  std::string code;  // e.g. "00700"
  Side side{Side::kBuy};
  bool sellShort{false};  // a sell that opens/extends a short must use TrdSide_SellShort
  std::int64_t qty{0};
  Money priceMills{0};  // limit price; market orders are deliberately not supported
  std::string remark;   // carries our ClOrdId so an ambiguous submit can be found again
};

struct PlacedOrder {
  std::uint64_t orderId{0};
  std::string orderIdEx;
};

struct BrokerOrder {
  std::uint64_t orderId{0};
  std::string orderIdEx;
  std::string code;
  Side side{Side::kBuy};
  std::int64_t qty{0};
  Money priceMills{0};
  std::int64_t fillQty{0};
  Money fillAvgPriceMills{0};
  std::int32_t status{0};  // raw Trd_Common.OrderStatus; map with oms::fromFutuStatus
  std::string remark;
  double updateTimestamp{0.0};
};

struct BrokerFill {
  std::string fillId;  // fillIDEx: unique per fill, used for idempotent application
  std::uint64_t orderId{0};
  std::string code;
  Side side{Side::kBuy};
  std::int64_t qty{0};
  Money priceMills{0};
  std::int32_t status{0};  // Trd_Common.OrderFillStatus: 0 OK, 1 cancelled (busted), ...
};

/** Pushed order status change (Trd_UpdateOrder). */
struct OrderUpdate {
  WireHeader header;
  BrokerOrder order;
};

/** Pushed fill (Trd_UpdateOrderFill). */
struct FillUpdate {
  WireHeader header;
  BrokerFill fill;
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
