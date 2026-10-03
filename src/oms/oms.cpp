#include "futu_trader/oms/oms.hpp"

#include "oms_impl.hpp"

namespace futu_trader::oms {

Oms::Oms(IVenue& venue, PreTradeRisk& risk, execution::RateLimiter& rate,
         execution::KillSwitch& killSwitch, portfolio::PositionBook& book, const Clock& clock,
         OmsConfig config)
    : impl_(std::make_unique<Impl>(venue, risk, rate, killSwitch, book, clock, std::move(config))) {
}

Oms::~Oms() = default;

void Oms::setJournalSink(std::function<void(const JournalEntry&)> sink) {
  std::scoped_lock lock(impl_->mu);
  impl_->sink = std::move(sink);
}

void Oms::setDurableSubmitSink(std::function<Result<bool>(const DurableSubmit&)> sink) {
  std::scoped_lock lock(impl_->mu);
  impl_->durableSink = std::move(sink);
}

const OmsStats& Oms::stats() const { return impl_->stats; }

std::size_t Oms::restoreIntents(const std::vector<DurableSubmit>& intents,
                                std::int64_t notBeforeNs) {
  auto& im = *impl_;
  std::scoped_lock lock(im.mu);
  std::size_t restored = 0;
  for (const auto& logged : intents) {
    if (logged.tsNs < notBeforeNs || !logged.clOrdId.starts_with(kIdPrefix) ||
        logged.intentKey.empty() || logged.symbol.empty() || im.records.contains(logged.clOrdId)) {
      continue;
    }
    OrderRecord rec;
    rec.clOrdId = logged.clOrdId;
    rec.intentKey = logged.intentKey;
    rec.symbol = logged.symbol;
    rec.side = logged.side;
    rec.qty = logged.qty;
    rec.priceMills = logged.priceMills;
    rec.state = OmsState::kUnknown;   // it may or may not exist at the broker: reconcile decides
    rec.sentAtNs = im.clock.nowNs();  // the grace period restarts now, not at the old send time
    rec.detail = "restored from write-ahead log";
    OrderRecord& stored = im.addRecord(std::move(rec));
    im.intentIndex[logged.intentKey] = stored.clOrdId;
    im.log(JournalKind::kAmbiguous, stored.clOrdId, "restored from write-ahead log after restart");
    ++restored;
  }
  im.stats.restoredIntents.fetch_add(restored);
  return restored;
}

Result<bool> Oms::bootstrap() {
  auto& im = *impl_;
  auto positions = im.venue.listPositions();
  if (!positions) {
    return positions.error();
  }
  auto orders = im.venue.listOrders();
  if (!orders) {
    return orders.error();
  }
  auto fills = im.venue.listFills();
  if (!fills) {
    return fills.error();
  }
  std::optional<Money> cash;
  if (im.config.cashToleranceMills) {
    auto funds = im.venue.funds();
    if (!funds) {
      return funds.error();
    }
    cash = funds.value().cash;
  }

  std::scoped_lock lock(im.mu);
  for (const auto& pos : positions.value()) {
    if (pos.qty == 0) {
      continue;
    }
    // Cost is informational for seeded positions; use the broker's cost price, else the mark.
    const Money cost = pos.costPrice > 0 ? pos.costPrice : pos.price;
    const auto seeded = im.book.seedPosition(pos.code, pos.qty, cost);
    if (!seeded) {
      return Error{ErrorCode::kInvalidArg,
                   "bootstrap: cannot seed " + pos.code + ": " + seeded.error().message};
    }
    if (pos.price > 0) {
      im.marks[pos.code] = {pos.price, im.clock.nowNs()};
    }
    im.log(JournalKind::kBootstrap, "", "seeded " + pos.code + " qty " + std::to_string(pos.qty));
  }
  // The seeded positions already include everything that filled before now. Remember those fills
  // as applied, or the first reconcile would apply them a second time and double the position.
  for (const auto& fill : fills.value()) {
    im.book.markFillSeen(fill.fillId);
  }
  for (const auto& order : orders.value()) {
    const auto state = fromFutuStatus(order.status).value_or(OmsState::kUnknown);
    if (!isTerminal(state) && im.byRemarkOrVenueId(order.remark, order.orderId) == nullptr) {
      im.adopt(order);
    }
  }
  im.baselineCash = cash;
  im.resetDailyBaselineLocked();  // an unrealised gain on a carried position is not a loss cushion
  im.bootstrapped = true;
  return true;
}

void Oms::onOrderUpdate(const opend::BrokerOrder& broker) {
  auto& im = *impl_;
  std::scoped_lock lock(im.mu);
  OrderRecord* rec = im.byRemarkOrVenueId(broker.remark, broker.orderId);
  if (rec == nullptr) {
    const auto state = fromFutuStatus(broker.status).value_or(OmsState::kUnknown);
    if (!isTerminal(state)) {
      im.adopt(broker);
    }
    return;
  }
  if (im.handlePresumedDead(*rec, broker)) {
    return;
  }
  im.observe(*rec, broker);
}

void Oms::onFill(const opend::BrokerFill& fill) {
  std::scoped_lock lock(impl_->mu);
  impl_->applyFillLocked(fill, nullptr);
}

void Oms::onMark(const std::string& symbol, Money priceMills) {
  if (priceMills <= 0) {
    return;
  }
  std::scoped_lock lock(impl_->mu);
  impl_->marks[symbol] = {priceMills, impl_->clock.nowNs()};
}

void Oms::resetDailyBaseline() {
  std::scoped_lock lock(impl_->mu);
  impl_->resetDailyBaselineLocked();
}

std::optional<OrderRecord> Oms::order(const std::string& clOrdId) const {
  std::scoped_lock lock(impl_->mu);
  const auto found = impl_->records.find(clOrdId);
  if (found == impl_->records.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<OrderRecord> Oms::orders() const {
  std::scoped_lock lock(impl_->mu);
  std::vector<OrderRecord> out;
  out.reserve(impl_->insertionOrder.size());
  for (const auto& id : impl_->insertionOrder) {
    out.push_back(impl_->records.at(id));
  }
  return out;
}

std::size_t Oms::liveOrderCount() const {
  std::scoped_lock lock(impl_->mu);
  return impl_->liveCount();
}

std::vector<std::string> Oms::liveOrderIds(const std::string& symbol) const {
  std::scoped_lock lock(impl_->mu);
  std::vector<std::string> ids;
  for (const auto& [liveSeq, rec] : impl_->live) {
    if (rec->symbol == symbol) {
      ids.push_back(rec->clOrdId);
    }
  }
  return ids;
}

bool Oms::hasLiveOrder(const std::string& symbol) const {
  std::scoped_lock lock(impl_->mu);
  return std::any_of(impl_->live.begin(), impl_->live.end(),
                     [&](const auto& kv) { return kv.second->symbol == symbol; });
}

std::size_t Oms::unresolvedCount() const {
  std::scoped_lock lock(impl_->mu);
  return static_cast<std::size_t>(
      std::count_if(impl_->live.begin(), impl_->live.end(),
                    [](const auto& kv) { return kv.second->state == OmsState::kUnknown; }));
}

std::size_t Oms::anomalyCount() const {
  std::scoped_lock lock(impl_->mu);
  return impl_->anomalies;
}

std::vector<JournalEntry> Oms::journal() const {
  std::scoped_lock lock(impl_->mu);
  return {impl_->journalLog.begin(), impl_->journalLog.end()};
}

}  // namespace futu_trader::oms
