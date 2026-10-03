#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/market/quote.hpp"
#include "futu_trader/oms/oms.hpp"

namespace futu_trader::strategy {

/**
 * What a strategy may do. It deliberately offers no way to read future data, the wall clock, or
 * the venue: time is `nowNs()`, randomness is `random()` (seeded, deterministic), and orders go
 * through the OMS (risk, rate limits, idempotency) exactly as in live trading.
 */
class StrategyContext {
 public:
  virtual ~StrategyContext() = default;

  virtual std::int64_t nowNs() const = 0;
  /** Signed position in our own book (long > 0, short < 0). */
  virtual std::int64_t position(const std::string& symbol) const = 0;
  virtual bool hasLiveOrder(const std::string& symbol) const = 0;
  virtual std::vector<std::string> liveOrderIds(const std::string& symbol) const = 0;
  /** Submits a limit order through the OMS. Prices are mills and must be on the tick grid. */
  virtual oms::SubmitResult submit(const std::string& symbol, Side side, std::int64_t qty,
                                   Money priceMills) = 0;
  virtual Result<bool> cancel(const std::string& clOrdId) = 0;
  virtual std::uint64_t random() = 0;
};

/** Event-driven strategy. It is called once per market event, in time order, and only then. */
class IStrategy {
 public:
  virtual ~IStrategy() = default;
  virtual void onQuote(const QuoteEvent& quote, StrategyContext& ctx) = 0;
};

}  // namespace futu_trader::strategy
