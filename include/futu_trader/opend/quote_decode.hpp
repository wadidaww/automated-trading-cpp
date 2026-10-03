#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "futu_trader/core/result.hpp"
#include "futu_trader/market/quote.hpp"
#include "futu_trader/opend/framing.hpp"

namespace futu_trader::opend {

/**
 * Turns OpenD market-data pushes into top-of-book QuoteEvents.
 *
 * The order-book push (3013) carries bid/ask and sizes but no last price; the basic-quote push
 * (3005) carries the last price. A QuoteEvent is emitted on each order-book push once the symbol's
 * last price is known, and never for a suspended symbol: a quote we cannot fully populate is not
 * emitted, because the pre-trade price band and staleness checks depend on a real last price
 * (fail closed). Prices are int64 mills; a crossed or empty book yields no event.
 *
 * The timestamp is the caller's own receive time, never OpenD's: our clock decides freshness.
 * Not thread-safe: call it from the single push-handling thread.
 */
class QuoteAssembler {
 public:
  /**
   * Feeds one push frame. Returns an event when the frame completes a quote. A frame that is not a
   * quote push, or is for a non-HK security, yields nullopt; a malformed one yields an error (and
   * leaves the symbol's state untouched).
   */
  Result<std::optional<QuoteEvent>> onFrame(const Frame& frame, std::int64_t recvNs);

  std::uint64_t ignoredNonHk() const { return ignoredNonHk_; }

 private:
  struct SymbolState {
    Money last{0};  // 0 = unknown or suspended
  };
  std::map<std::string, SymbolState> symbols_;
  std::uint64_t ignoredNonHk_{0};
};

}  // namespace futu_trader::opend
