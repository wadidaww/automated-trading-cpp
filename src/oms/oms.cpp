#include "futu_trader/oms/oms.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>

#include "futu_trader/oms/journal_codec.hpp"

namespace futu_trader::oms {

namespace {

__extension__ using Int128 = __int128;

constexpr const char* kIdPrefix = "FT-";
constexpr std::size_t kMaxIntentField = 256;  // the journal codec caps fields well above this
constexpr std::int64_t kNsPerMs = 1'000'000;

Money saturate(Int128 value) {
  if (value > INT64_MAX) {
    return INT64_MAX;
  }
  if (value < INT64_MIN) {
    return INT64_MIN;
  }
  return static_cast<Money>(value);
}

// Errors that prove the broker did NOT create the order. Everything else is ambiguous.
bool definitiveRefusal(const Error& error) {
  return error.code == ErrorCode::kServer || error.code == ErrorCode::kInvalidArg;
}

std::optional<OmsEvent> eventFor(OmsState observed) {
  switch (observed) {
    case OmsState::kWorking:
      return OmsEvent::kAcked;
    case OmsState::kPartiallyFilled:
      return OmsEvent::kPartialFill;
    case OmsState::kFilled:
      return OmsEvent::kFullFill;
    case OmsState::kCancelPending:
      return OmsEvent::kCancelRequested;
    case OmsState::kCancelled:
      return OmsEvent::kCancelConfirmed;
    case OmsState::kRejected:
      return OmsEvent::kRejected;
    case OmsState::kNew:
    case OmsState::kPendingSubmit:
    case OmsState::kUnknown:
      return std::nullopt;
  }
  return std::nullopt;
}

struct Mark {
  Money price{0};
  std::int64_t atNs{0};
};

}  // namespace

struct Oms::Impl {
  Impl(IVenue& v, PreTradeRisk& r, execution::RateLimiter& rl, execution::KillSwitch& ks,
       portfolio::PositionBook& b, const Clock& c, OmsConfig cfg)
      : venue(v), risk(r), rate(rl), kill(ks), book(b), clock(c), config(std::move(cfg)) {
    records.reserve(config.reserveOrders);
    intentIndex.reserve(config.reserveOrders);
    venueIndex.reserve(config.reserveOrders);
    insertionOrder.reserve(config.reserveOrders);
    book.reserveFills(config.reserveOrders * 2);  // typically 1-2 fills per order
  }

  IVenue& venue;
  PreTradeRisk& risk;
  execution::RateLimiter& rate;
  execution::KillSwitch& kill;
  portfolio::PositionBook& book;
  const Clock& clock;
  OmsConfig config;

  mutable std::mutex mu;
  bool bootstrapped{false};
  std::uint64_t seq{0};
  std::unordered_map<std::string, OrderRecord> records;
  std::vector<std::string> insertionOrder;
  // Live orders only, keyed by creation sequence (so iteration order is deterministic and equals
  // submission order). Everything that used to scan ALL orders ever placed (risk view, self-trade,
  // unresolved checks, halt, reconcile) now scans this small set: submit cost no longer grows with
  // the day's order history. Records are never erased, so these pointers stay valid.
  std::map<std::uint64_t, OrderRecord*> live;
  std::uint64_t recordSeq{0};
  std::unordered_map<std::string, std::string> intentIndex;   // intentKey -> clOrdId
  std::unordered_map<std::uint64_t, std::string> venueIndex;  // venueOrderId -> clOrdId
  std::unordered_map<std::string, Mark> marks;
  std::size_t anomalies{0};
  int failedReconciles{0};
  Money dailyBaseline{0};
  std::optional<Money> baselineCash;
  std::deque<JournalEntry> journalLog;
  std::function<void(const JournalEntry&)> sink;
  std::function<Result<bool>(const DurableSubmit&)> durableSink;
  OmsStats stats;
  // Set by any automatic trip; a thread that may talk to the venue then cancels everything.
  std::atomic<bool> haltRequested{false};

  // ---- helpers (call with mu held unless noted) ----

  void log(JournalKind kind, const std::string& clOrdId, const std::string& detail) {
    JournalEntry entry{clock.nowNs(), kind, clOrdId, detail};
    if (sink) {
      sink(entry);
    }
    journalLog.push_back(std::move(entry));
    if (journalLog.size() > config.journalRetain) {
      journalLog.pop_front();
    }
  }

  void anomaly(const std::string& clOrdId, const std::string& detail) {
    ++anomalies;
    log(JournalKind::kAnomaly, clOrdId, detail);
  }

  // Every automatic halt goes through here so it always also schedules the cancel-all.
  void autoTrip(const std::string& reason) {
    kill.trip(reason);
    haltRequested.store(true);
    log(JournalKind::kHalt, "", reason);
  }

  // Keeps the live index in step with the record's state. Called wherever state can change.
  void trackLive(OrderRecord& rec) {
    if (isLive(rec.state)) {
      live[rec.seq] = &rec;
    } else {
      live.erase(rec.seq);
    }
  }

  OrderRecord& addRecord(OrderRecord record) {
    const std::string id = record.clOrdId;
    record.seq = ++recordSeq;
    const auto [slot, inserted] = records.emplace(id, std::move(record));
    if (inserted) {
      insertionOrder.push_back(id);
      trackLive(slot->second);
    } else {
      anomaly(id, "duplicate order id ignored");  // must never happen: ids are minted unique
    }
    return slot->second;
  }

  OrderRecord* byRemarkOrVenueId(const std::string& remark, std::uint64_t venueId) {
    if (!remark.empty()) {
      const auto found = records.find(remark);
      if (found != records.end()) {
        return &found->second;
      }
    }
    if (venueId != 0) {
      const auto found = venueIndex.find(venueId);
      if (found != venueIndex.end()) {
        return &records.at(found->second);
      }
    }
    return nullptr;
  }

  void setState(OrderRecord& rec, OmsState next, const std::string& why) {
    if (rec.state == next) {
      return;
    }
    log(JournalKind::kStateChange, rec.clOrdId,
        std::string(toString(rec.state)) + " -> " + toString(next) + " (" + why + ")");
    rec.state = next;
    trackLive(rec);
  }

  bool fire(OrderRecord& rec, OmsEvent event, const std::string& why) {
    const auto next = transition(rec.state, event);
    if (!next) {
      anomaly(rec.clOrdId,
              std::string("illegal transition from ") + toString(rec.state) + ": " + why);
      return false;
    }
    setState(rec, *next, why);
    return true;
  }

  // Applies what the broker says about an order. Returns true if our state changed.
  bool observe(OrderRecord& rec, const opend::BrokerOrder& broker) {
    if (broker.orderId != 0 && rec.venueOrderId == 0) {
      rec.venueOrderId = broker.orderId;
      venueIndex[broker.orderId] = rec.clOrdId;
    }
    rec.filledQty = std::max(rec.filledQty, broker.fillQty);  // fills only ever accumulate

    const auto observed = fromFutuStatus(broker.status);
    if (!observed) {
      anomaly(rec.clOrdId, "unrecognised broker status " + std::to_string(broker.status));
      rec.detail = "unrecognised broker status";
      return false;
    }
    if (*observed == OmsState::kUnknown) {
      anomaly(rec.clOrdId, "broker reported status " + std::to_string(broker.status));
      rec.detail = "broker reported unclear status";
      return false;
    }
    if (*observed == rec.state || *observed == OmsState::kPendingSubmit) {
      return false;
    }
    // We asked for a cancel a while ago, yet the broker still shows the order as working: the
    // cancel never took effect. Go back to working so the next cancel actually resends it.
    if (*observed == OmsState::kWorking && rec.state == OmsState::kCancelPending &&
        clock.nowNs() - rec.cancelSentAtNs >= config.cancelRetryMs * kNsPerMs) {
      fire(rec, OmsEvent::kCancelRejected, "cancel not effective at broker");
      if (rec.filledQty > 0 && rec.state == OmsState::kWorking) {
        setState(rec, OmsState::kPartiallyFilled, "cancel not effective, order has fills");
      }
      return true;
    }
    const auto event = eventFor(*observed);
    if (!event) {
      return false;
    }
    const OmsState before = rec.state;
    const auto next = transition(rec.state, *event);
    if (!next) {
      // Stale or duplicate update (e.g. "working" arriving after "partially filled"): ignore.
      anomaly(rec.clOrdId,
              std::string("ignored ") + toString(*observed) + " while " + toString(rec.state));
      return false;
    }
    setState(rec, *next, "broker update");
    return before != rec.state;
  }

  std::size_t liveCount() const { return live.size(); }

  std::size_t liveWithVenueId() const {
    std::size_t count = 0;
    for (const auto& [liveSeq, rec] : live) {
      count += rec->venueOrderId != 0 ? 1U : 0U;
    }
    return count;
  }

  Int128 unrealizedLocked() const {
    Int128 unrealized = 0;
    for (const auto& pos : book.all()) {
      if (pos.qty == 0) {
        continue;
      }
      const std::int64_t absQty = std::llabs(pos.qty);
      const auto mark = marks.find(pos.symbol);
      const Money price = mark != marks.end() ? mark->second.price : pos.costBasis / absQty;
      const Int128 marketValue = static_cast<Int128>(absQty) * price;
      unrealized += pos.qty > 0 ? marketValue - pos.costBasis : pos.costBasis - marketValue;
    }
    return unrealized;
  }

  void resetDailyBaselineLocked() {
    dailyBaseline = saturate(static_cast<Int128>(book.totalRealizedPnl()) - book.totalFees() +
                             unrealizedLocked());
  }

  struct RiskView {
    AccountRiskState state;
    bool marksStale{false};  // a held symbol has no fresh mark: P&L and exposure are unreliable
  };

  // Positions, resting orders and P&L as the risk chain needs them. Resting orders on the same
  // side as the candidate are counted as exposure (otherwise N resting orders could each pass a
  // cap that they breach together), and resting sells are netted out of what we can sell.
  RiskView riskView(const OrderIntent& intent) const {
    RiskView view;
    AccountRiskState& state = view.state;
    const std::int64_t nowNs = clock.nowNs();
    Int128 gross = 0;
    for (const auto& pos : book.all()) {
      if (pos.qty == 0) {
        continue;
      }
      const std::int64_t absQty = std::llabs(pos.qty);
      const auto mark = marks.find(pos.symbol);
      if (mark == marks.end() || nowNs - mark->second.atNs > config.markMaxAgeMs * kNsPerMs) {
        view.marksStale = true;
      }
      const Money price = mark != marks.end() ? mark->second.price : pos.costBasis / absQty;
      const Int128 value = static_cast<Int128>(pos.qty) * price;
      state.exposure[pos.symbol] = saturate(value);
      gross += value < 0 ? -value : value;
    }
    const std::int64_t sign = sideSign(intent.side);
    Int128 sameSidePending = 0;
    std::int64_t pendingSells = 0;
    for (const auto& [liveSeq, recPtr] : live) {
      const OrderRecord& rec = *recPtr;
      const std::int64_t remainingQty = std::max<std::int64_t>(0, rec.qty - rec.filledQty);
      const Int128 notional = static_cast<Int128>(remainingQty) * rec.priceMills;
      gross += notional;
      if (rec.symbol == intent.symbol && rec.side == intent.side) {
        sameSidePending += notional;
      }
      if (rec.symbol == intent.symbol && rec.side == Side::kSell) {
        pendingSells += remainingQty;
      }
    }
    state.exposure[intent.symbol] =
        saturate(static_cast<Int128>(state.exposure[intent.symbol]) + (sign * sameSidePending));
    state.grossNotional = saturate(gross);
    state.dailyPnl = saturate(static_cast<Int128>(book.totalRealizedPnl()) - book.totalFees() +
                              unrealizedLocked() - dailyBaseline);
    state.liveOrders = liveCount();
    state.heldQty = book.qty(intent.symbol) - pendingSells;
    return view;
  }

  bool symbolHasUnresolved(const std::string& symbol) const {
    return std::any_of(live.begin(), live.end(), [&](const auto& kv) {
      return kv.second->symbol == symbol && kv.second->state == OmsState::kUnknown;
    });
  }

  // Would this order trade against one of our own resting orders in the same symbol?
  bool wouldSelfTrade(const OrderIntent& intent) const {
    return std::any_of(live.begin(), live.end(), [&](const auto& kv) {
      const OrderRecord& rec = *kv.second;
      if (rec.symbol != intent.symbol || rec.side == intent.side) {
        return false;
      }
      return intent.side == Side::kBuy ? intent.priceMills >= rec.priceMills
                                       : intent.priceMills <= rec.priceMills;
    });
  }

  void applyFillLocked(const opend::BrokerFill& fill, std::size_t* applied) {
    if (fill.status != 0) {
      // A busted/changed fill invalidates positions in a way we cannot replay: stop and escalate.
      anomaly("", "fill " + fill.fillId + " reported with status " + std::to_string(fill.status));
      autoTrip("busted or changed fill " + fill.fillId + ": manual reconciliation required");
      return;
    }
    const Int128 turnover = static_cast<Int128>(fill.priceMills) * fill.qty;
    if (turnover > INT64_MAX) {
      anomaly("", "fill turnover overflow " + fill.fillId);
      autoTrip("fill turnover overflow");
      return;
    }
    const Money fee = portfolio::hkFee(config.fees, static_cast<Money>(turnover));
    const auto result =
        book.applyFill({fill.fillId, fill.code, fill.side, fill.qty, fill.priceMills, fee});
    if (!result) {
      anomaly("", "could not apply fill " + fill.fillId + ": " + result.error().message);
      autoTrip("fill could not be applied: " + fill.fillId);
      return;
    }
    if (result.value()) {
      if (applied != nullptr) {
        ++*applied;
      }
      log(JournalKind::kFill, "",
          fill.code + (fill.side == Side::kBuy ? " buy " : " sell ") + std::to_string(fill.qty) +
              " @" + std::to_string(fill.priceMills) + " id " + fill.fillId);
    }
  }

  // Adopts a live broker order that we have no record of (placed by a previous run or by hand).
  OrderRecord& adopt(const opend::BrokerOrder& broker, const std::string& forcedId = "") {
    OrderRecord rec;
    const bool ours = broker.remark.starts_with(kIdPrefix);
    if (!forcedId.empty()) {
      rec.clOrdId = forcedId;
    } else if (ours) {
      rec.clOrdId = broker.remark;
    } else {
      rec.clOrdId = "EXT-" + std::to_string(broker.orderId);
    }
    rec.symbol = broker.code;
    rec.side = broker.side;
    rec.qty = broker.qty;
    rec.priceMills = broker.priceMills;
    rec.venueOrderId = broker.orderId;
    rec.filledQty = broker.fillQty;
    rec.external = forcedId.empty() ? !ours : true;
    rec.state = fromFutuStatus(broker.status).value_or(OmsState::kUnknown);
    if (rec.state == OmsState::kPendingSubmit) {
      rec.state = OmsState::kWorking;
    }
    venueIndex[broker.orderId] = rec.clOrdId;
    log(JournalKind::kBootstrap, rec.clOrdId,
        std::string("adopted broker order in state ") + toString(rec.state));
    return addRecord(std::move(rec));
  }

  // An order we declared "never existed" (absent from repeated listings) is being reported by the
  // broker after all. The intent may already have been retried, so a duplicate could be live:
  // stop trading, and make sure the late order is tracked (and therefore cancelled by the halt).
  // Returns true if `rec` was a presumed-dead record (the caller should not process it further).
  bool handlePresumedDead(OrderRecord& rec, const opend::BrokerOrder& broker) {
    if (!rec.presumedDead) {
      return false;
    }
    const auto observed = fromFutuStatus(broker.status);
    const bool benign = observed &&
                        (*observed == OmsState::kRejected || *observed == OmsState::kCancelled) &&
                        broker.fillQty == 0;
    if (benign) {
      return true;
    }
    const std::string lateId = "LATE-" + std::to_string(broker.orderId);
    const auto existing = records.find(lateId);
    if (existing != records.end()) {
      observe(existing->second, broker);
      return true;
    }
    anomaly(rec.clOrdId, "order presumed dead reappeared at broker as " + lateId);
    adopt(broker, lateId);
    autoTrip("order " + rec.clOrdId + " was presumed dead but exists at the broker: possible " +
             "duplicate of intent " + rec.intentKey);
    return true;
  }

  // Cancels one order. `force` resends even if a cancel is already pending and recent.
  // Calls the venue with no lock held.
  Result<bool> cancelOrder(const std::string& clOrdId, bool force) {
    std::uint64_t venueId = 0;
    {
      std::scoped_lock lock(mu);
      const auto found = records.find(clOrdId);
      if (found == records.end()) {
        return Error{ErrorCode::kInvalidArg, "unknown order " + clOrdId};
      }
      OrderRecord& rec = found->second;
      if (!isLive(rec.state)) {
        return Error{ErrorCode::kInvalidArg, "order is not live"};
      }
      if (rec.venueOrderId == 0) {
        return Error{ErrorCode::kInvalidArg,
                     "order has no venue id yet (unresolved): reconcile before cancelling"};
      }
      const std::int64_t now = clock.nowNs();
      if (rec.state == OmsState::kCancelPending && !force &&
          now - rec.cancelSentAtNs < config.cancelRetryMs * kNsPerMs) {
        return true;  // asked recently: give the broker time to answer
      }
      if (!rate.tryAcquire(execution::RequestKind::kCancel)) {
        return Error{ErrorCode::kTimeout, "rate limited"};
      }
      if (rec.state != OmsState::kCancelPending) {
        fire(rec, OmsEvent::kCancelRequested, "cancel requested");
      }
      rec.cancelSentAtNs = now;
      log(JournalKind::kCancelRequested, rec.clOrdId, force ? "forced" : "");
      venueId = rec.venueOrderId;
    }

    const auto result = venue.cancel(venueId);

    std::scoped_lock lock(mu);
    OrderRecord& rec = records.at(clOrdId);
    if (result) {
      return true;  // confirmation arrives as an order update (or via reconcile)
    }
    if (definitiveRefusal(result.error()) && rec.state == OmsState::kCancelPending) {
      // Broker refused the cancel; the order is still where it was.
      fire(rec, OmsEvent::kCancelRejected, result.error().message);
      if (rec.filledQty > 0 && rec.state == OmsState::kWorking) {
        setState(rec, OmsState::kPartiallyFilled, "cancel refused, order has fills");
      }
    }
    // Ambiguous failure: stay CancelPending; it will be retried after cancelRetryMs.
    return result.error();
  }

  HaltReport runHalt(bool force) {
    HaltReport report;
    std::vector<std::string> targets;
    {
      std::scoped_lock lock(mu);
      for (const auto& [liveSeq, recPtr] : live) {
        const OrderRecord& rec = *recPtr;
        if (rec.venueOrderId == 0) {
          ++report.unresolvedWithoutVenueId;
        } else {
          targets.push_back(rec.clOrdId);
        }
      }
    }
    for (const auto& id : targets) {
      ++report.cancelRequested;
      if (!cancelOrder(id, force)) {
        ++report.cancelFailed;
      }
    }
    std::scoped_lock lock(mu);
    if (liveWithVenueId() == 0) {
      haltRequested.store(false);  // nothing left that we can (or need to) cancel
    }
    return report;
  }
};

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

SubmitResult Oms::submit(const OrderIntent& intent, const QuoteContext& callerQuote) {
  if (impl_->haltRequested.load()) {
    serviceHalt();  // finish any pending cancel-all before deciding anything else
  }
  SubmitResult result = submitInner(intent, callerQuote);
  impl_->stats.submits[static_cast<std::size_t>(result.status)].fetch_add(1);
  if (result.status == SubmitStatus::kRejectedByRisk) {
    const auto reason = static_cast<std::size_t>(result.risk);
    impl_->stats.riskRejects[std::min(reason, impl_->stats.riskRejects.size() - 1)].fetch_add(1);
  }
  if (impl_->haltRequested.load()) {
    serviceHalt();  // this very call may have tripped a halt (loss limit, halt during place)
  }
  return result;
}

SubmitResult Oms::submitInner(const OrderIntent& intent, const QuoteContext& callerQuote) {
  auto& im = *impl_;
  SubmitResult out;
  Order order;
  order.symbol = intent.symbol;
  order.side = intent.side;
  order.quantity = intent.qty;
  order.limitPriceMinor = intent.priceMills;
  opend::PlaceOrderRequest request;
  std::function<Result<bool>(const DurableSubmit&)> durable;
  DurableSubmit durableRecord;
  {
    std::scoped_lock lock(im.mu);
    if (!im.bootstrapped) {
      out.status = SubmitStatus::kNotReady;
      out.detail = "bootstrap() has not completed";
      return out;
    }
    if (intent.intentKey.size() > kMaxIntentField || intent.symbol.size() > kMaxIntentField) {
      out.status = SubmitStatus::kInvalid;
      out.detail = "intent key or symbol too long";
      return out;
    }
    if (intent.intentKey.empty() || intent.symbol.empty()) {
      out.status = SubmitStatus::kInvalid;
      out.detail = "intent key and symbol are required";
      return out;
    }
    const auto known = im.intentIndex.find(intent.intentKey);
    if (known != im.intentIndex.end()) {
      const OrderRecord& previous = im.records.at(known->second);
      out.clOrdId = previous.clOrdId;
      if (previous.state == OmsState::kUnknown) {
        out.status = SubmitStatus::kBlockedUnresolved;
        out.detail = "an earlier submit of this intent has an unknown outcome";
        return out;
      }
      if (previous.state != OmsState::kRejected || previous.presumedDead) {
        // A presumed-dead intent stays blocked too: it may still turn up at the broker.
        out.status = SubmitStatus::kDuplicate;
        return out;
      }
      im.intentIndex.erase(known);  // a definitively rejected intent may be retried
    }

    // The caller supplies the quote's price and receive time, but "now" is OUR clock: a replayed
    // quote must not be able to vouch for its own freshness.
    QuoteContext quote = callerQuote;
    quote.nowNs = im.clock.nowNs();
    if (im.risk.quoteIsFresh(quote)) {
      im.marks[intent.symbol] = {quote.lastPriceMills, quote.nowNs};
    }

    const std::int64_t held = im.book.qty(intent.symbol);
    const bool reducing = (held > 0 && intent.side == Side::kSell && intent.qty <= held) ||
                          (held < 0 && intent.side == Side::kBuy && intent.qty <= -held);
    Impl::RiskView view = im.riskView(intent);
    RiskReject verdict = RiskReject::kOk;
    if (im.symbolHasUnresolved(intent.symbol)) {
      verdict = RiskReject::kUnresolvedOrder;  // don't stack orders on an unknown outcome
    } else if (im.wouldSelfTrade(intent)) {
      verdict = RiskReject::kSelfTrade;
    } else if (view.marksStale && !reducing) {
      verdict = RiskReject::kStaleQuote;  // P&L is unreliable: only allow risk-reducing orders
    } else {
      verdict = im.risk.check(order, quote, view.state);
    }
    if (verdict != RiskReject::kOk) {
      out.status = SubmitStatus::kRejectedByRisk;
      out.risk = verdict;
      out.detail = toString(verdict);
      im.log(JournalKind::kRiskReject, "", intent.intentKey + ": " + toString(verdict));
      if (verdict == RiskReject::kDailyLoss) {
        im.autoTrip("daily loss limit reached");  // a loss limit halts trading, not just this order
      }
      return out;
    }
    if (!im.rate.tryAcquire(execution::RequestKind::kNewOrder)) {
      out.status = SubmitStatus::kRejectedByRisk;
      out.risk = RiskReject::kRateLimit;
      out.detail = toString(RiskReject::kRateLimit);
      im.log(JournalKind::kRiskReject, "", intent.intentKey + ": rate_limit");
      return out;
    }

    OrderRecord rec;
    // Skip ids that already exist: a broker push (or a restart that reuses the epoch) can have
    // adopted a record under an id we would otherwise mint next.
    do {
      rec.clOrdId =
          std::string(kIdPrefix) + im.config.sessionEpoch + "-" + std::to_string(++im.seq);
    } while (im.records.contains(rec.clOrdId));
    rec.intentKey = intent.intentKey;
    rec.symbol = intent.symbol;
    rec.side = intent.side;
    rec.qty = intent.qty;
    rec.priceMills = intent.priceMills;
    rec.sentAtNs = im.clock.nowNs();
    OrderRecord& stored = im.addRecord(std::move(rec));
    im.intentIndex[intent.intentKey] = stored.clOrdId;
    im.fire(stored, OmsEvent::kSubmitSent, "submit");
    im.log(JournalKind::kSubmitRequested, stored.clOrdId,
           intent.symbol + " qty " + std::to_string(intent.qty) + " @" +
               std::to_string(intent.priceMills));
    out.clOrdId = stored.clOrdId;

    request.code = intent.symbol;
    request.side = intent.side;
    // Net of resting sells: two sells that together exceed the holding are the second one a short.
    request.sellShort = intent.side == Side::kSell && intent.qty > view.state.heldQty;
    request.qty = intent.qty;
    request.priceMills = intent.priceMills;
    request.remark = stored.clOrdId;
    durable = im.durableSink;
    durableRecord = {stored.sentAtNs, stored.clOrdId, intent.intentKey, intent.symbol,
                     intent.side,     intent.qty,     intent.priceMills};
  }

  // Write-ahead: the intent must be on disk before the order can exist at the broker. No lock held
  // (an fdatasync can take milliseconds). If it cannot be made durable, nothing was sent, so this
  // is a definite non-event: reject, release the key, and halt (the log is no longer trustworthy).
  if (durable) {
    const auto logged = durable(durableRecord);
    if (!logged) {
      im.stats.durableFailures.fetch_add(1);
      std::scoped_lock lock(im.mu);
      OrderRecord& rec = im.records.at(out.clOrdId);
      if (rec.state == OmsState::kPendingSubmit) {
        im.fire(rec, OmsEvent::kRejected, "write-ahead log failed");
      }
      rec.detail = "write-ahead log failed: " + logged.error().message;
      im.intentIndex.erase(intent.intentKey);
      im.autoTrip("write-ahead log failure");
      out.status = SubmitStatus::kNotDurable;
      out.detail = rec.detail;
      return out;
    }
  }

  // Network call with no lock held.
  const auto placed = im.venue.place(request);

  {
    std::scoped_lock lock(im.mu);
    OrderRecord& rec = im.records.at(out.clOrdId);
    if (placed) {
      rec.venueOrderId = placed.value().orderId;
      im.venueIndex[rec.venueOrderId] = rec.clOrdId;
      if (rec.state == OmsState::kPendingSubmit) {
        im.fire(rec, OmsEvent::kAcked, "venue accepted");
      }  // else a push already advanced it: keep the more advanced state
      im.log(JournalKind::kPlaced, rec.clOrdId, "venue order " + std::to_string(rec.venueOrderId));
      out.status = SubmitStatus::kAccepted;
    } else {
      const Error& error = placed.error();
      out.detail = error.message;
      if (definitiveRefusal(error)) {
        if (rec.state == OmsState::kPendingSubmit) {
          im.fire(rec, OmsEvent::kRejected, error.message);
        }
        rec.detail = error.message;
        im.log(JournalKind::kVenueReject, rec.clOrdId, error.message);
        out.status = SubmitStatus::kRejectedByVenue;
      } else {
        // Timeout / disconnect / garbled response: the order may exist. Never assume it does not.
        if (rec.state == OmsState::kPendingSubmit) {
          im.fire(rec, OmsEvent::kSubmitAmbiguous, error.message);
        }
        rec.detail = error.message;
        im.log(JournalKind::kAmbiguous, rec.clOrdId, error.message);
        out.status = SubmitStatus::kAmbiguous;
      }
    }
    // A halt that landed while place() was in flight was decided before this order existed at
    // the broker: make sure it gets cancelled too.
    if (im.kill.tripped()) {
      im.haltRequested.store(true);
    }
  }
  return out;
}

Result<bool> Oms::cancel(const std::string& clOrdId) { return impl_->cancelOrder(clOrdId, false); }

HaltReport Oms::haltAndCancelAll(const std::string& reason) {
  auto& im = *impl_;
  {
    std::scoped_lock lock(im.mu);
    im.autoTrip(reason);
  }
  return im.runHalt(true);
}

HaltReport Oms::cancelAllLive() {
  auto& im = *impl_;
  HaltReport report;
  std::vector<std::string> targets;
  {
    std::scoped_lock lock(im.mu);
    for (const auto& [liveSeq, recPtr] : im.live) {
      if (recPtr->venueOrderId == 0) {
        ++report.unresolvedWithoutVenueId;
      } else {
        targets.push_back(recPtr->clOrdId);
      }
    }
  }
  for (const auto& id : targets) {
    ++report.cancelRequested;
    if (!im.cancelOrder(id, true)) {
      ++report.cancelFailed;
    }
  }
  return report;
}

void Oms::requestHalt(const std::string& reason) {
  std::scoped_lock lock(impl_->mu);
  impl_->autoTrip(reason);
}

HaltReport Oms::serviceHalt() {
  if (!impl_->haltRequested.load()) {
    return {};
  }
  return impl_->runHalt(false);
}

bool Oms::haltPending() const { return impl_->haltRequested.load(); }

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

ReconcileReport Oms::reconcile() {
  auto& im = *impl_;
  ReconcileReport report;

  const auto fail = [&](const std::string& why) {
    report.error = why;
    {
      std::scoped_lock lock(im.mu);
      ++im.failedReconciles;
      im.log(JournalKind::kReconcile, "", "incomplete: " + why);
      if (im.failedReconciles >= im.config.maxFailedReconciles) {
        im.autoTrip("reconciliation failed " + std::to_string(im.failedReconciles) +
                    " times in a row: " + why);
        report.halted = true;
      }
    }
    if (report.halted) {
      serviceHalt();
    }
    return report;
  };

  // Anything we send after this instant may legitimately be missing from the list we fetch now.
  const std::int64_t fetchStartNs = im.clock.nowNs();
  auto orders = im.venue.listOrders();
  if (!orders) {
    return fail("order list: " + orders.error().message);
  }

  // 1) Orders: catch up state, resolve ambiguous submits, adopt strangers.
  {
    std::scoped_lock lock(im.mu);
    std::set<std::string> seen;
    for (const auto& broker : orders.value()) {
      OrderRecord* rec = im.byRemarkOrVenueId(broker.remark, broker.orderId);
      if (rec == nullptr) {
        const auto state = fromFutuStatus(broker.status).value_or(OmsState::kUnknown);
        if (!isTerminal(state)) {
          const OrderRecord& adopted = im.adopt(broker);
          seen.insert(adopted.clOrdId);
          ++report.adoptedExternal;
        }
        continue;
      }
      seen.insert(rec->clOrdId);
      if (im.handlePresumedDead(*rec, broker)) {
        report.drifts.push_back({DriftKind::kOrderMissingAtBroker,
                                 rec->clOrdId + " was presumed dead but the broker lists it"});
        continue;
      }
      rec->missedListings = 0;
      const bool wasUnknown = rec->state == OmsState::kUnknown;
      const auto observed = fromFutuStatus(broker.status);
      if (observed && *observed == OmsState::kUnknown) {
        report.drifts.push_back(
            {DriftKind::kOrderStatusUnknown,
             rec->clOrdId + ": broker status " + std::to_string(broker.status)});
      }
      if (im.observe(*rec, broker)) {
        ++report.statusCatchUps;
        if (wasUnknown) {
          ++report.resolvedUnknown;
        }
      }
    }
    const std::int64_t now = im.clock.nowNs();
    // Snapshot: the loop below may change states (and so the live index) as it goes.
    std::vector<OrderRecord*> liveNow;
    liveNow.reserve(im.live.size());
    for (const auto& [liveSeq, recPtr] : im.live) {
      liveNow.push_back(recPtr);
    }
    for (OrderRecord* recPtr : liveNow) {
      OrderRecord& rec = *recPtr;
      if (!isLive(rec.state) || seen.contains(rec.clOrdId)) {
        continue;
      }
      if (rec.sentAtNs > fetchStartNs) {
        continue;  // sent after we fetched the list: absence proves nothing
      }
      if (rec.venueOrderId == 0 || rec.state == OmsState::kUnknown ||
          rec.state == OmsState::kPendingSubmit) {
        // Ambiguous submit that the broker does not list. "Never existed" needs BOTH the grace
        // period and several consecutive complete listings without it: one list can be behind.
        ++rec.missedListings;
        if (now - rec.sentAtNs >= im.config.ambiguityGraceMs * kNsPerMs &&
            rec.missedListings >= im.config.absenceListingsRequired) {
          if (rec.state == OmsState::kPendingSubmit) {
            im.fire(rec, OmsEvent::kSubmitAmbiguous, "not listed after grace");
          }
          im.fire(rec, OmsEvent::kRejected, "not found at broker after grace period");
          rec.detail = "not found at broker after grace period";
          rec.presumedDead = true;  // if it ever shows up, that is an incident, not a resolution
          ++report.resolvedUnknown;
        }
        continue;
      }
      report.drifts.push_back(
          {DriftKind::kOrderMissingAtBroker,
           rec.clOrdId + " (" + toString(rec.state) + ") is not at the broker"});
    }
  }

  // 2) Fills and positions. A fill can land between the two queries, so re-check once before
  //    calling a mismatch real.
  bool positionsOk = false;
  for (int attempt = 0; attempt < 2 && !positionsOk; ++attempt) {
    auto fills = im.venue.listFills();
    if (!fills) {
      return fail("fill list: " + fills.error().message);
    }
    auto positions = im.venue.listPositions();
    if (!positions) {
      return fail("position list: " + positions.error().message);
    }
    std::scoped_lock lock(im.mu);
    for (const auto& fill : fills.value()) {
      im.applyFillLocked(fill, &report.missedFillsApplied);
    }
    std::unordered_map<std::string, std::int64_t> brokerQty;
    for (const auto& pos : positions.value()) {
      brokerQty[pos.code] = pos.qty;
    }
    std::vector<Drift> mismatches;
    std::set<std::string> symbols;
    for (const auto& pos : im.book.all()) {
      symbols.insert(pos.symbol);
    }
    for (const auto& [code, qty] : brokerQty) {
      symbols.insert(code);
    }
    for (const auto& symbol : symbols) {
      const std::int64_t ours = im.book.qty(symbol);
      const auto theirs = brokerQty.find(symbol);
      const std::int64_t broker = theirs == brokerQty.end() ? 0 : theirs->second;
      if (ours != broker) {
        mismatches.push_back(
            {DriftKind::kPositionMismatch,
             symbol + ": ours " + std::to_string(ours) + " broker " + std::to_string(broker)});
      }
    }
    if (mismatches.empty()) {
      positionsOk = true;
    } else if (attempt == 1) {
      report.drifts.insert(report.drifts.end(), mismatches.begin(), mismatches.end());
    }
  }

  // 3) Cash (opt-in): broker cash should equal baseline plus what our fills moved.
  if (im.config.cashToleranceMills) {
    auto funds = im.venue.funds();
    if (!funds) {
      return fail("funds: " + funds.error().message);
    }
    std::scoped_lock lock(im.mu);
    if (im.baselineCash) {
      const Int128 expected = static_cast<Int128>(*im.baselineCash) + im.book.cashDelta();
      const Int128 diff = expected - funds.value().cash;
      if ((diff < 0 ? -diff : diff) > *im.config.cashToleranceMills) {
        report.drifts.push_back(
            {DriftKind::kCashMismatch, "expected " + std::to_string(saturate(expected)) +
                                           " broker " + std::to_string(funds.value().cash)});
      }
    }
  }

  report.complete = true;
  {
    std::scoped_lock lock(im.mu);
    im.failedReconciles = 0;
    im.log(JournalKind::kReconcile, "",
           "complete: drifts " + std::to_string(report.drifts.size()) + ", missed fills " +
               std::to_string(report.missedFillsApplied) + ", catch-ups " +
               std::to_string(report.statusCatchUps));
    if (!report.drifts.empty()) {
      im.autoTrip("reconciliation drift: " + report.drifts.front().detail);
      report.halted = true;
    }
  }
  if (im.haltRequested.load()) {
    serviceHalt();  // a drift halt must also pull resting orders, not just stop new ones
  }
  return report;
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
