#pragma once

// PRIVATE to the OMS translation units (oms.cpp, oms_submit.cpp, oms_reconcile.cpp): the state and
// the lock-held helpers behind the public Oms facade. Not part of the include/ interface.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>

#include "futu_trader/oms/journal_codec.hpp"
#include "futu_trader/oms/oms.hpp"

namespace futu_trader::oms {

namespace detail {

__extension__ using Int128 = __int128;

inline constexpr const char* kIdPrefix = "FT-";
inline constexpr std::size_t kMaxIntentField =
    256;  // the journal codec caps fields well above this
inline constexpr std::int64_t kNsPerMs = 1'000'000;

inline Money saturate(Int128 value) {
  if (value > INT64_MAX) {
    return INT64_MAX;
  }
  if (value < INT64_MIN) {
    return INT64_MIN;
  }
  return static_cast<Money>(value);
}

// Errors that prove the broker did NOT create the order. Everything else is ambiguous.
inline bool definitiveRefusal(const Error& error) {
  return error.code == ErrorCode::kServer || error.code == ErrorCode::kInvalidArg;
}

inline std::optional<OmsEvent> eventFor(OmsState observed) {
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

}  // namespace detail

using namespace detail;

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
      if (*event == OmsEvent::kAcked) {
        // A late "working" after we already moved on (cancel requested, partly filled...) is just a
        // push that overtook nothing: normal at a real gateway, so it is counted but is not an
        // anomaly.
        stats.staleUpdates.fetch_add(1);
        return false;
      }
      // Any other update that cannot apply (e.g. "cancelled" while filled) is a real inconsistency.
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

  // ---- reconciliation phases (defined in oms_reconcile.cpp) ----
  // Each takes the lock itself; the two that query the broker do it with no lock held. The ones
  // that can fail return the failure text, which makes the whole reconciliation "incomplete".
  void reconcileOrders(const std::vector<opend::BrokerOrder>& orders, std::int64_t fetchStartNs,
                       ReconcileReport& report);
  std::optional<std::string> reconcilePositions(ReconcileReport& report);
  std::optional<std::string> reconcileCash(ReconcileReport& report);
  void concludeReconcile(ReconcileReport& report);

  // ---- the submit pipeline (defined in oms_submit.cpp) ----
  // What the locked admission phase hands to the phases that run with no lock held.
  struct Admission {
    std::string clOrdId;
    opend::PlaceOrderRequest request;
    std::function<Result<bool>(const DurableSubmit&)> durable;  // empty if no sink is installed
    DurableSubmit logged;
  };
  // Locked. An engaged result ends the submit; nullopt means "continue" (adm is then filled).
  std::optional<SubmitResult> admit(const OrderIntent& intent, const QuoteContext& callerQuote,
                                    Admission& adm);
  std::optional<SubmitResult> validate(const OrderIntent& intent) const;
  std::optional<SubmitResult> dedupe(const OrderIntent& intent);
  std::optional<SubmitResult> screen(const OrderIntent& intent, const QuoteContext& quote,
                                     const RiskView& view);
  OrderRecord& createRecord(const OrderIntent& intent);
  // Unlocked entry points: each takes the lock itself where it must touch state.
  SubmitResult failDurable(const OrderIntent& intent, const std::string& clOrdId,
                           const Error& error);
  SubmitResult recordVenueReply(const std::string& clOrdId,
                                const Result<opend::PlacedOrder>& placed);

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

}  // namespace futu_trader::oms
