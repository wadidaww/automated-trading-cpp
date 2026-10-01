#include "futu_trader/backtest/sim_venue.hpp"

#include <algorithm>
#include <cstdlib>

namespace futu_trader::backtest {

namespace {

__extension__ using Int128 = __int128;

constexpr std::int32_t kStatusSubmitted = 5;
constexpr std::int32_t kStatusFilledPart = 10;
constexpr std::int32_t kStatusFilledAll = 11;
constexpr std::int32_t kStatusCancelledPart = 14;
constexpr std::int32_t kStatusCancelledAll = 15;

Error refuse(const std::string& why) { return Error{ErrorCode::kServer, "sim broker: " + why}; }

}  // namespace

SimVenue::SimVenue(SimVenueConfig config, const instrument::InstrumentTable& instruments,
                   ManualClock& clock)
    : config_(config), instruments_(instruments), clock_(clock) {}

void SimVenue::setSinks(OrderSink orderSink, FillSink fillSink) {
  orderSink_ = std::move(orderSink);
  fillSink_ = std::move(fillSink);
}

Money SimVenue::reservedBuyMills() const {
  Int128 total = 0;
  for (const auto& [id, order] : orders_) {
    if (order.terminal || order.pub.side != Side::kBuy) {
      continue;
    }
    const Int128 notional = static_cast<Int128>(order.remaining) * order.pub.priceMills;
    total += notional + portfolio::hkFee(config_.fees, static_cast<Money>(notional));
  }
  return total > INT64_MAX ? INT64_MAX : static_cast<Money>(total);
}

std::int64_t SimVenue::reservedSellQty(const std::string& symbol) const {
  std::int64_t total = 0;
  for (const auto& [id, order] : orders_) {
    if (!order.terminal && order.pub.side == Side::kSell && order.pub.code == symbol) {
      total += order.remaining;
    }
  }
  return total;
}

Result<opend::PlacedOrder> SimVenue::place(const opend::PlaceOrderRequest& request) {
  if (request.qty <= 0 || request.priceMills <= 0) {
    return refuse("invalid quantity or price");
  }
  const auto info = instruments_.find(request.code);
  if (!info || info->lotSize <= 0) {
    return refuse("unknown instrument " + request.code);
  }
  if (request.qty % info->lotSize != 0) {
    return refuse("quantity is not a board-lot multiple");
  }
  if (!instrument::isTickAligned(request.priceMills)) {
    return refuse("price is not on the tick grid");
  }
  if (request.sellShort && !config_.allowShort) {
    return refuse("short selling is not enabled");
  }

  const Int128 notional = static_cast<Int128>(request.priceMills) * request.qty;
  if (notional > INT64_MAX) {
    return refuse("order value out of range");
  }
  if (request.side == Side::kBuy) {
    const Money fee = portfolio::hkFee(config_.fees, static_cast<Money>(notional));
    const Int128 available = static_cast<Int128>(cash()) - reservedBuyMills();
    if (available < notional + fee) {
      return refuse("insufficient buying power");
    }
  } else if (!config_.allowShort) {
    const std::int64_t sellable = truth_.qty(request.code) - reservedSellQty(request.code);
    if (request.qty > sellable) {
      return refuse("insufficient sellable position");
    }
  }

  SimOrder order;
  order.pub.orderId = nextOrderId_++;
  order.pub.orderIdEx = "SIM" + std::to_string(order.pub.orderId);
  order.pub.code = request.code;
  order.pub.side = request.side;
  order.pub.qty = request.qty;
  order.pub.priceMills = request.priceMills;
  order.pub.status = kStatusSubmitted;
  order.pub.remark = request.remark;
  order.pub.updateTimestamp = static_cast<double>(clock_.nowNs()) / 1e9;
  order.remaining = request.qty;
  order.sellShort = request.sellShort;
  const std::uint64_t id = order.pub.orderId;
  const std::string idEx = order.pub.orderIdEx;
  orders_.emplace(id, std::move(order));
  schedule(clock_.nowNs() + config_.latencyNs, ActionKind::kActivate, id);
  return opend::PlacedOrder{id, idEx};
}

Result<bool> SimVenue::cancel(std::uint64_t venueOrderId) {
  const auto found = orders_.find(venueOrderId);
  if (found == orders_.end()) {
    return refuse("no such order");
  }
  if (found->second.terminal) {
    return refuse("order is no longer cancellable");
  }
  schedule(clock_.nowNs() + config_.cancelLatencyNs, ActionKind::kCancel, venueOrderId);
  return true;  // accepted; whether it wins the race against a fill is decided when it lands
}

void SimVenue::schedule(std::int64_t atNs, ActionKind kind, std::uint64_t orderId) {
  schedule_.emplace(std::make_pair(atNs, nextSeq_++), Action{kind, orderId});
}

std::int64_t SimVenue::nextActionNs() const {
  return schedule_.empty() ? -1 : schedule_.begin()->first.first;
}

void SimVenue::advance(std::int64_t nowNs) {
  while (!schedule_.empty() && schedule_.begin()->first.first <= nowNs) {
    const auto entry = schedule_.begin();
    const std::int64_t at = entry->first.first;
    const Action action = entry->second;
    schedule_.erase(entry);
    if (at > clock_.nowNs()) {
      clock_.setNs(at);  // the world's time when this action happens
    }
    const auto found = orders_.find(action.orderId);
    if (found == orders_.end() || found->second.terminal) {
      continue;
    }
    SimOrder& order = found->second;
    if (action.kind == ActionKind::kActivate) {
      order.active = true;
      emitOrder(order);
      const auto quote = quotes_.find(order.pub.code);
      if (quote != quotes_.end()) {
        tryMatch(order, quote->second, /*arriving=*/true);
      }
    } else {
      order.terminal = true;
      order.pub.status = order.pub.fillQty > 0 ? kStatusCancelledPart : kStatusCancelledAll;
      emitOrder(order);
    }
  }
}

void SimVenue::onQuote(const QuoteEvent& quote) {
  const auto previous = quotes_.find(quote.symbol);
  Consumed& used = consumed_[quote.symbol];
  if (previous == quotes_.end() || previous->second.ask != quote.ask ||
      previous->second.askSize != quote.askSize) {
    used.ask = 0;  // genuinely new liquidity at the ask
  }
  if (previous == quotes_.end() || previous->second.bid != quote.bid ||
      previous->second.bidSize != quote.bidSize) {
    used.bid = 0;
  }
  quotes_[quote.symbol] = quote;
  for (auto& [id, order] : orders_) {
    if (order.active && !order.terminal && order.pub.code == quote.symbol) {
      tryMatch(order, quote, /*arriving=*/false);
    }
  }
}

void SimVenue::tryMatch(SimOrder& order, const QuoteEvent& quote, bool arriving) {
  const bool buy = order.pub.side == Side::kBuy;
  const Money limit = order.pub.priceMills;
  const Money touch = buy ? quote.ask : quote.bid;
  const std::int64_t displayed = buy ? quote.askSize : quote.bidSize;
  if (touch <= 0) {
    return;
  }
  const bool marketable = buy ? limit >= touch : limit <= touch;
  bool fills = false;
  Money price = touch;
  if (arriving) {
    fills = marketable;  // arrives already crossing: takes the touch price
    order.aggressive = marketable;
  } else if (order.aggressive) {
    fills = marketable;  // the unfilled remainder of a taker order keeps taking the touch
  } else if (config_.passiveNeedsTradeThrough) {
    fills = buy ? touch < limit : touch > limit;
    price = limit;  // a resting order trades at its own price
  } else {
    fills = marketable;
    price = limit;
  }
  if (!fills) {
    return;
  }
  std::int64_t& used = buy ? consumed_[order.pub.code].ask : consumed_[order.pub.code].bid;
  std::int64_t available = order.remaining;
  if (displayed > 0) {
    available = std::min(order.remaining, displayed - used);
  } else if (!config_.sizeZeroMeansUnlimited) {
    available = 0;  // unknown liquidity is not evidence of any
  }
  if (available > 0) {
    used += available;
    fill(order, available, price);
  }
}

void SimVenue::fill(SimOrder& order, std::int64_t qty, Money price) {
  opend::BrokerFill fill;
  fill.fillId = "SIM-" + std::to_string(nextFillId_++);
  fill.orderId = order.pub.orderId;
  fill.code = order.pub.code;
  fill.side = order.pub.side;
  fill.qty = qty;
  fill.priceMills = price;
  fill.status = 0;

  const Int128 turnover = static_cast<Int128>(price) * qty;
  const Money fee = portfolio::hkFee(config_.fees, static_cast<Money>(turnover));
  // The broker's own books are the ground truth the OMS must agree with.
  (void)truth_.applyFill({fill.fillId, fill.code, fill.side, qty, price, fee});
  fills_.push_back(fill);

  const std::int64_t before = order.pub.fillQty;
  order.pub.fillQty = before + qty;
  const Int128 weighted = (static_cast<Int128>(order.pub.fillAvgPriceMills) * before) + turnover;
  order.pub.fillAvgPriceMills = static_cast<Money>(weighted / order.pub.fillQty);
  order.remaining -= qty;
  if (order.remaining == 0) {
    order.terminal = true;
    order.pub.status = kStatusFilledAll;
  } else {
    order.pub.status = kStatusFilledPart;
  }
  order.pub.updateTimestamp = static_cast<double>(clock_.nowNs()) / 1e9;
  if (fillSink_) {
    fillSink_(fill);
  }
  emitOrder(order);
}

void SimVenue::emitOrder(const SimOrder& order) {
  if (orderSink_) {
    orderSink_(order.pub);
  }
}

Result<std::vector<opend::BrokerOrder>> SimVenue::listOrders() {
  std::vector<opend::BrokerOrder> out;
  out.reserve(orders_.size());
  for (const auto& [id, order] : orders_) {
    out.push_back(order.pub);
  }
  return out;
}

Result<std::vector<opend::BrokerFill>> SimVenue::listFills() { return fills_; }

Result<std::vector<opend::PositionInfo>> SimVenue::listPositions() {
  std::vector<opend::PositionInfo> out;
  for (const auto& pos : truth_.all()) {
    if (pos.qty == 0) {
      continue;
    }
    const std::int64_t absQty = std::llabs(pos.qty);
    const Money cost = pos.costBasis / absQty;
    const auto quote = quotes_.find(pos.symbol);
    const Money price = quote != quotes_.end() ? quote->second.mid() : cost;
    const std::int64_t sellable =
        pos.qty > 0 ? std::max<std::int64_t>(0, pos.qty - reservedSellQty(pos.symbol)) : 0;
    out.push_back({pos.symbol, pos.qty, sellable, cost, price});
  }
  return out;
}

Result<opend::FundsInfo> SimVenue::funds() {
  opend::FundsInfo info;
  info.cash = cash();
  info.frozenCash = reservedBuyMills();
  info.power = info.cash - info.frozenCash;
  std::map<std::string, Money> marks;
  for (const auto& [symbol, quote] : quotes_) {
    marks[symbol] = quote.mid();
  }
  info.totalAssets = equity(marks);
  info.marketValue = info.totalAssets - info.cash;
  return info;
}

Money SimVenue::cash() const { return config_.initialCashMills + truth_.cashDelta(); }

std::int64_t SimVenue::position(const std::string& symbol) const { return truth_.qty(symbol); }

Money SimVenue::equity(const std::map<std::string, Money>& marks) const {
  Int128 total = cash();
  for (const auto& pos : truth_.all()) {
    if (pos.qty == 0) {
      continue;
    }
    const auto mark = marks.find(pos.symbol);
    const Money price = mark != marks.end() ? mark->second : pos.costBasis / std::llabs(pos.qty);
    total += static_cast<Int128>(pos.qty) * price;
  }
  if (total > INT64_MAX) {
    return INT64_MAX;
  }
  if (total < INT64_MIN) {
    return INT64_MIN;
  }
  return static_cast<Money>(total);
}

}  // namespace futu_trader::backtest
