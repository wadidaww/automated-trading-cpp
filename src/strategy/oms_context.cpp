#include "futu_trader/strategy/oms_context.hpp"

namespace futu_trader::strategy {

void OmsContext::observe(const QuoteEvent& quote) {
  Seen& seen = seen_[quote.symbol];  // no allocation once the symbol has been seen
  seen.mid = quote.mid();
  seen.receivedNs = clock_.nowNs();
}

oms::SubmitResult OmsContext::submit(const std::string& symbol, Side side, std::int64_t qty,
                                     Money priceMills) {
  oms::QuoteContext quote;
  const auto found = seen_.find(symbol);
  if (found != seen_.end()) {
    quote.lastPriceMills = found->second.mid;
    quote.quoteTimeNs = found->second.receivedNs;
  }
  quote.nowNs = clock_.nowNs();
  const oms::OrderIntent intent{keyPrefix_ + std::to_string(++counter_), symbol, side, qty, priceMills};
  const oms::SubmitResult result = oms_.submit(intent, quote);
  if (observer_) {
    observer_(quote.nowNs, symbol, side, qty, priceMills, result);
  }
  return result;
}

}  // namespace futu_trader::strategy
