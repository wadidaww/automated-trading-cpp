#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace futu_trader {

using Money = std::int64_t;
using TimePoint = std::chrono::system_clock::time_point;
using FeatureVector = std::vector<double>;

enum class SignalAction : uint8_t { kBuy, kSell, kHold };

enum class Side : uint8_t { kBuy, kSell };

/** Futu trading environment. Paper trading is kSimulate; kReal moves real money. */
enum class TrdEnv : uint8_t { kSimulate, kReal };

/** Signed direction multiplier: +1 for buy, -1 for sell. */
constexpr std::int64_t sideSign(Side side) { return side == Side::kBuy ? 1 : -1; }

struct Signal {
  SignalAction action{SignalAction::kHold};
  double confidence{0.0};
};

struct Tick {
  std::string symbol;
  Money priceMinor{0};
  std::int64_t volume{0};
  TimePoint timestamp;
};

enum class OrderType : uint8_t { kMarket, kLimit, kStopLimit, kTrailingStop };
enum class OrderState : uint8_t {
  kPending,
  kSubmitted,
  kPartialFill,
  kFilled,
  kCancelled,
  kRejected
};

struct Order {
  std::string orderId;
  std::string symbol;
  Side side{Side::kBuy};
  std::int64_t quantity{0};  // always positive; direction is carried by `side`
  Money limitPriceMinor{0};
  OrderType type{OrderType::kLimit};
  OrderState state{OrderState::kPending};
  std::string idempotencyKey;
};

struct TradeSignal {
  std::string symbol;
  Signal signal;
  std::int64_t suggestedQuantity{0};
  TimePoint timestamp;
};

}  // namespace futu_trader
