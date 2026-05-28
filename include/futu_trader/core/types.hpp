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

struct Signal {
  SignalAction action{SignalAction::kHold};
  double confidence{0.0};
};

struct Tick {
  std::string symbol;
  Money priceMinor{0};
  std::int64_t volume{0};
  TimePoint timestamp{};
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
  std::int64_t quantity{0};
  Money limitPriceMinor{0};
  OrderType type{OrderType::kLimit};
  OrderState state{OrderState::kPending};
  std::string idempotencyKey;
};

struct TradeSignal {
  std::string symbol;
  Signal signal;
  std::int64_t suggestedQuantity{0};
  TimePoint timestamp{};
};

}  // namespace futu_trader
