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

/** What the pre-trade checks look at: one order's economics. Direction is carried by `side`. */
struct Order {
  std::string symbol;
  Side side{Side::kBuy};
  std::int64_t quantity{0};  // always positive
  Money limitPriceMinor{0};
};

}  // namespace futu_trader
