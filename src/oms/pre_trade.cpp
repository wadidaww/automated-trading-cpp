#include "futu_trader/oms/pre_trade.hpp"

#include <cstdlib>

namespace futu_trader::oms {

namespace {
__extension__ using Int128 = __int128;
}

PreTradeRisk::PreTradeRisk(RiskConfig limits, PreTradeConfig config,
                           const execution::KillSwitch& killSwitch,
                           const instrument::InstrumentTable& instruments)
    : engine_(limits), config_(config), killSwitch_(killSwitch), instruments_(instruments) {}

bool PreTradeRisk::quoteIsFresh(const QuoteContext& quote) const {
  return quote.lastPriceMills > 0 && quote.nowNs >= quote.quoteTimeNs &&
         quote.nowNs - quote.quoteTimeNs <= config_.maxQuoteAgeMs * 1'000'000;
}

RiskReject PreTradeRisk::check(const Order& order, const QuoteContext& quote,
                               const AccountRiskState& state) const {
  if (killSwitch_.tripped()) {
    return RiskReject::kKillSwitch;
  }
  if (order.quantity <= 0 || order.limitPriceMinor <= 0) {
    return RiskReject::kInvalidOrder;
  }

  const auto info = instruments_.find(order.symbol);
  if (!info || info->lotSize <= 0) {
    return RiskReject::kUnknownInstrument;
  }
  if (order.quantity % info->lotSize != 0) {
    return RiskReject::kLotSize;
  }
  if (!instrument::isTickAligned(order.limitPriceMinor)) {
    return RiskReject::kTickSize;
  }

  // Fail closed on missing market data: without a fresh reference price the band is meaningless.
  if (!quoteIsFresh(quote)) {
    return RiskReject::kStaleQuote;
  }
  const auto diff = static_cast<Int128>(std::llabs(order.limitPriceMinor - quote.lastPriceMills));
  if (diff * 10'000 > static_cast<Int128>(quote.lastPriceMills) * config_.priceBandBps) {
    return RiskReject::kPriceBand;
  }

  const Int128 notional = static_cast<Int128>(order.limitPriceMinor) * order.quantity;
  if (notional > config_.maxOrderNotionalMills) {
    return RiskReject::kMaxOrderNotional;
  }

  if (order.side == Side::kSell && !config_.allowShort && order.quantity > state.heldQty) {
    return RiskReject::kShortSale;
  }

  return engine_.check(order, state.exposure, state.grossNotional, state.dailyPnl,
                       state.liveOrders);
}

}  // namespace futu_trader::oms
