#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "futu_trader/core/types.hpp"
#include "futu_trader/execution/kill_switch.hpp"
#include "futu_trader/instrument/hk_rules.hpp"
#include "futu_trader/risk/risk_engine.hpp"

namespace futu_trader::oms {

struct PreTradeConfig {
  std::int64_t priceBandBps{200};    // max |price - last| / last, in basis points
  std::int64_t maxQuoteAgeMs{2000};  // reject if the last quote is older than this
  Money maxOrderNotionalMills{0};    // fat-finger cap per order; 0 rejects everything
  bool allowShort{false};            // selling more than held requires this AND broker support
};

struct QuoteContext {
  Money lastPriceMills{0};
  std::int64_t quoteTimeNs{0};  // Clock time when the quote was received
  std::int64_t nowNs{0};
};

/** Everything the risk chain needs to know about the account right now. */
struct AccountRiskState {
  std::unordered_map<std::string, Money> exposure;  // signed notional per symbol
  Money grossNotional{0};
  Money dailyPnl{0};
  std::size_t liveOrders{0};
  std::int64_t heldQty{0};  // signed position in this order's symbol
};

/**
 * Full pre-trade chain. Every check is O(1) and allocation-free on the accept path, runs before
 * any request leaves the process, and yields a reason code. Fails closed: missing data (no quote,
 * unknown instrument, unset limit) rejects rather than passes.
 */
class PreTradeRisk {
 public:
  PreTradeRisk(RiskConfig limits, PreTradeConfig config, const execution::KillSwitch& killSwitch,
               const instrument::InstrumentTable& instruments);

  RiskReject check(const Order& order, const QuoteContext& quote,
                   const AccountRiskState& state) const;

  /** True if the quote is usable as a reference price right now (present and not stale). */
  bool quoteIsFresh(const QuoteContext& quote) const;

 private:
  RiskEngine engine_;
  PreTradeConfig config_;
  const execution::KillSwitch& killSwitch_;
  const instrument::InstrumentTable& instruments_;
};

}  // namespace futu_trader::oms
