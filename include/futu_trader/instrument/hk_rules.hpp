#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "futu_trader/core/types.hpp"

namespace futu_trader::instrument {

/**
 * HKEX equity spread (tick size) table. Prices and ticks are int64 mills (1/1000 HKD).
 *
 * Bands are lower-bound inclusive. The table is DATA, versioned by kHkSpreadTableVersion: verify
 * it against the current HKEX schedule before trading and bump the version when it changes.
 * ETFs, warrants and other products use different tables and are not covered.
 */
inline constexpr const char* kHkSpreadTableVersion =
    "hkex-equity-2018-onwards (verify before live)";

/** Tick size in mills for a price, or 0 if the price is outside the tradable range. */
Money hkEquityTick(Money priceMills);

/** True if the price is inside the range and an exact multiple of its tick. */
bool isTickAligned(Money priceMills);

struct InstrumentInfo {
  std::string code;         // e.g. "00700"
  std::int64_t lotSize{0};  // board lot; 0 means unknown (orders are rejected)
};

/** Board lots are per security and change over time, so they come from data, never constants. */
class InstrumentTable {
 public:
  void add(InstrumentInfo info);
  std::optional<InstrumentInfo> find(const std::string& code) const;

 private:
  std::unordered_map<std::string, InstrumentInfo> byCode_;
};

}  // namespace futu_trader::instrument
