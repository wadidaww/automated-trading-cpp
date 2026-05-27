#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace futu_trader {

using Money = std::int64_t;
using TimePoint = std::chrono::system_clock::time_point;
using FeatureVector = std::vector<double>;

enum class SignalAction { kBuy, kSell, kHold };

struct Signal {
  SignalAction action{SignalAction::kHold};
  double confidence{0.0};
};

struct Tick {
  std::string symbol;
  Money price_minor{0};
  std::int64_t volume{0};
  TimePoint timestamp{};
};

enum class OrderType { kMarket, kLimit, kStopLimit, kTrailingStop };
enum class OrderState { kPending, kSubmitted, kPartialFill, kFilled, kCancelled, kRejected };

struct Order {
  std::string order_id;
  std::string symbol;
  std::int64_t quantity{0};
  Money limit_price_minor{0};
  OrderType type{OrderType::kLimit};
  OrderState state{OrderState::kPending};
  std::string idempotency_key;
};

struct TradeSignal {
  std::string symbol;
  Signal signal;
  std::int64_t suggested_quantity{0};
  TimePoint timestamp{};
};

}  // namespace futu_trader
