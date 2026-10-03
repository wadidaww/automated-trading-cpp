#include "futu_trader/oms/oms.hpp"
#include "oms_impl.hpp"

namespace futu_trader::oms {

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
  Impl::Admission adm;
  {
    std::scoped_lock lock(im.mu);
    if (auto early = im.admit(intent, callerQuote, adm)) {
      return *early;
    }
  }
  // Write-ahead: the intent must be on disk before the order can exist at the broker. No lock held
  // (an fdatasync can take milliseconds).
  if (adm.durable) {
    if (const auto logged = adm.durable(adm.logged); !logged) {
      return im.failDurable(intent, adm.clOrdId, logged.error());
    }
  }
  const auto placed = im.venue.place(adm.request);  // network call, no lock held
  return im.recordVenueReply(adm.clOrdId, placed);
}

std::optional<SubmitResult> Oms::Impl::validate(const OrderIntent& intent) const {
  SubmitResult out;
  if (!bootstrapped) {
    out.status = SubmitStatus::kNotReady;
    out.detail = "bootstrap() has not completed";
  } else if (intent.intentKey.size() > kMaxIntentField || intent.symbol.size() > kMaxIntentField) {
    out.status = SubmitStatus::kInvalid;
    out.detail = "intent key or symbol too long";
  } else if (intent.intentKey.empty() || intent.symbol.empty()) {
    out.status = SubmitStatus::kInvalid;
    out.detail = "intent key and symbol are required";
  } else {
    return std::nullopt;
  }
  return out;
}

// Idempotency: one intent key never yields two orders.
std::optional<SubmitResult> Oms::Impl::dedupe(const OrderIntent& intent) {
  const auto known = intentIndex.find(intent.intentKey);
  if (known == intentIndex.end()) {
    return std::nullopt;
  }
  const OrderRecord& previous = records.at(known->second);
  SubmitResult out;
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
  intentIndex.erase(known);  // a definitively rejected intent may be retried
  return std::nullopt;
}

// The pre-trade chain: unresolved orders, self-trade, stale marks, the risk checks, then the rate
// budget. A risk-reducing order is exempt from the unresolved and stale-mark blocks (rule 12:
// flattening must always be possible).
std::optional<SubmitResult> Oms::Impl::screen(const OrderIntent& intent, const QuoteContext& quote,
                                              const RiskView& view) {
  const std::int64_t held = book.qty(intent.symbol);
  const bool reducing = (held > 0 && intent.side == Side::kSell && intent.qty <= held) ||
                        (held < 0 && intent.side == Side::kBuy && intent.qty <= -held);
  Order order;
  order.symbol = intent.symbol;
  order.side = intent.side;
  order.quantity = intent.qty;
  order.limitPriceMinor = intent.priceMills;

  RiskReject verdict = RiskReject::kOk;
  if (symbolHasUnresolved(intent.symbol) && !reducing) {
    verdict = RiskReject::kUnresolvedOrder;  // don't stack exposure on an unknown outcome
  } else if (wouldSelfTrade(intent)) {
    verdict = RiskReject::kSelfTrade;
  } else if (view.marksStale && !reducing) {
    verdict = RiskReject::kStaleQuote;  // P&L is unreliable: only allow risk-reducing orders
  } else {
    verdict = risk.check(order, quote, view.state);
  }
  SubmitResult out;
  out.status = SubmitStatus::kRejectedByRisk;
  if (verdict != RiskReject::kOk) {
    out.risk = verdict;
    out.detail = toString(verdict);
    log(JournalKind::kRiskReject, "", intent.intentKey + ": " + toString(verdict));
    if (verdict == RiskReject::kDailyLoss) {
      autoTrip("daily loss limit reached");  // a loss limit halts trading, not just this order
    }
    return out;
  }
  if (!rate.tryAcquire(execution::RequestKind::kNewOrder)) {
    out.risk = RiskReject::kRateLimit;
    out.detail = toString(RiskReject::kRateLimit);
    log(JournalKind::kRiskReject, "", intent.intentKey + ": rate_limit");
    return out;
  }
  return std::nullopt;
}

OrderRecord& Oms::Impl::createRecord(const OrderIntent& intent) {
  OrderRecord rec;
  // Skip ids that already exist: a broker push (or a restart that reuses the epoch) can have
  // adopted a record under an id we would otherwise mint next.
  do {
    rec.clOrdId = std::string(kIdPrefix) + config.sessionEpoch + "-" + std::to_string(++seq);
  } while (records.contains(rec.clOrdId));
  rec.intentKey = intent.intentKey;
  rec.symbol = intent.symbol;
  rec.side = intent.side;
  rec.qty = intent.qty;
  rec.priceMills = intent.priceMills;
  rec.sentAtNs = clock.nowNs();
  OrderRecord& stored = addRecord(std::move(rec));
  intentIndex[intent.intentKey] = stored.clOrdId;
  fire(stored, OmsEvent::kSubmitSent, "submit");
  log(JournalKind::kSubmitRequested, stored.clOrdId,
      intent.symbol + " qty " + std::to_string(intent.qty) + " @" +
          std::to_string(intent.priceMills));
  return stored;
}

std::optional<SubmitResult> Oms::Impl::admit(const OrderIntent& intent,
                                             const QuoteContext& callerQuote, Admission& adm) {
  if (auto bad = validate(intent)) {
    return bad;
  }
  if (auto repeat = dedupe(intent)) {
    return repeat;
  }
  // The caller supplies the quote's price and receive time, but "now" is OUR clock: a replayed
  // quote must not be able to vouch for its own freshness.
  QuoteContext quote = callerQuote;
  quote.nowNs = clock.nowNs();
  if (risk.quoteIsFresh(quote)) {
    marks[intent.symbol] = {quote.lastPriceMills, quote.nowNs};
  }
  const RiskView view = riskView(intent);
  if (auto refused = screen(intent, quote, view)) {
    return refused;
  }

  OrderRecord& stored = createRecord(intent);
  adm.clOrdId = stored.clOrdId;
  adm.request.code = intent.symbol;
  adm.request.side = intent.side;
  // Net of resting sells: two sells that together exceed the holding are the second one a short.
  adm.request.sellShort = intent.side == Side::kSell && intent.qty > view.state.heldQty;
  adm.request.qty = intent.qty;
  adm.request.priceMills = intent.priceMills;
  adm.request.remark = stored.clOrdId;
  adm.durable = durableSink;
  adm.logged = {stored.sentAtNs, stored.clOrdId, intent.intentKey, intent.symbol,
                intent.side,     intent.qty,     intent.priceMills};
  if (!adm.durable && config.requireDurable) {
    // Fail closed: this process was told it must write ahead, and nobody installed the sink.
    fire(stored, OmsEvent::kRejected, "no write-ahead sink installed");
    intentIndex.erase(intent.intentKey);
    SubmitResult out;
    out.status = SubmitStatus::kNotDurable;
    out.clOrdId = stored.clOrdId;
    out.detail = "write-ahead sink required but not installed";
    return out;
  }
  return std::nullopt;
}

// The intent could not be made durable, so nothing was sent: a definite non-event. Reject, release
// the key, and halt (a log that cannot be written cannot protect a restart).
SubmitResult Oms::Impl::failDurable(const OrderIntent& intent, const std::string& clOrdId,
                                    const Error& error) {
  stats.durableFailures.fetch_add(1);
  std::scoped_lock lock(mu);
  OrderRecord& rec = records.at(clOrdId);
  if (rec.state == OmsState::kPendingSubmit) {
    fire(rec, OmsEvent::kRejected, "write-ahead log failed");
  }
  rec.detail = "write-ahead log failed: " + error.message;
  intentIndex.erase(intent.intentKey);
  autoTrip("write-ahead log failure");
  SubmitResult out;
  out.status = SubmitStatus::kNotDurable;
  out.clOrdId = clOrdId;
  out.detail = rec.detail;
  return out;
}

// Applies the broker's answer to the order. Only a definitive refusal (rule 10) proves the order
// does not exist; anything else leaves it Unknown until reconciled.
SubmitResult Oms::Impl::recordVenueReply(const std::string& clOrdId,
                                         const Result<opend::PlacedOrder>& placed) {
  std::scoped_lock lock(mu);
  OrderRecord& rec = records.at(clOrdId);
  SubmitResult out;
  out.clOrdId = clOrdId;
  if (placed) {
    rec.venueOrderId = placed.value().orderId;
    venueIndex[rec.venueOrderId] = rec.clOrdId;
    if (rec.state == OmsState::kPendingSubmit) {
      fire(rec, OmsEvent::kAcked, "venue accepted");
    }  // else a push already advanced it: keep the more advanced state
    log(JournalKind::kPlaced, rec.clOrdId, "venue order " + std::to_string(rec.venueOrderId));
    out.status = SubmitStatus::kAccepted;
  } else {
    const Error& error = placed.error();
    const bool refused = definitiveRefusal(error);
    if (rec.state == OmsState::kPendingSubmit) {
      fire(rec, refused ? OmsEvent::kRejected : OmsEvent::kSubmitAmbiguous, error.message);
    }
    rec.detail = error.message;
    log(refused ? JournalKind::kVenueReject : JournalKind::kAmbiguous, rec.clOrdId, error.message);
    out.detail = error.message;
    out.status = refused ? SubmitStatus::kRejectedByVenue : SubmitStatus::kAmbiguous;
  }
  // A halt that landed while place() was in flight was decided before this order existed at the
  // broker: make sure it gets cancelled too.
  if (kill.tripped()) {
    haltRequested.store(true);
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
      if (recPtr->external) {
        continue;  // a planned stop must not cancel a human's manual orders (a halt still does)
      }
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

}  // namespace futu_trader::oms
