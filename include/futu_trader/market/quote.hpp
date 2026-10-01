#pragma once

#include <cstdint>
#include <string>

#include "futu_trader/core/types.hpp"

namespace futu_trader {

/** Top-of-book snapshot. Prices are int64 mills; sizes are shares (0 = unknown/unlimited). */
struct QuoteEvent {
  std::int64_t tsNs{0};  // nanoseconds since the Unix epoch (UTC)
  std::string symbol;
  Money bid{0};
  Money ask{0};
  Money last{0};
  std::int64_t bidSize{0};
  std::int64_t askSize{0};

  /** Midpoint, used as a mark price. Integer division truncates toward zero. */
  Money mid() const { return (bid + ask) / 2; }
};

inline bool operator==(const QuoteEvent& a, const QuoteEvent& b) {
  return a.tsNs == b.tsNs && a.symbol == b.symbol && a.bid == b.bid && a.ask == b.ask &&
         a.last == b.last && a.bidSize == b.bidSize && a.askSize == b.askSize;
}

}  // namespace futu_trader
