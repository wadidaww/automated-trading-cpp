#include "futu_trader/oms/oms.hpp"
#include "oms_impl.hpp"

namespace futu_trader::oms {

ReconcileReport Oms::reconcile() {
  auto& im = *impl_;
  ReconcileReport report;

  // An incomplete reconciliation draws no conclusions; repeated ones trip the kill switch.
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
  const auto orders = im.venue.listOrders();
  if (!orders) {
    return fail("order list: " + orders.error().message);
  }
  im.reconcileOrders(orders.value(), fetchStartNs, report);
  if (const auto why = im.reconcilePositions(report)) {
    return fail(*why);
  }
  if (const auto why = im.reconcileCash(report)) {
    return fail(*why);
  }
  im.concludeReconcile(report);
  if (im.haltRequested.load()) {
    serviceHalt();  // a drift halt must also pull resting orders, not just stop new ones
  }
  return report;
}

// 1) Orders: catch up state, resolve ambiguous submits, adopt strangers.
void Oms::Impl::reconcileOrders(const std::vector<opend::BrokerOrder>& brokerOrders,
                                std::int64_t fetchStartNs, ReconcileReport& report) {
  std::scoped_lock lock(mu);
  std::set<std::string> seen;
  for (const auto& broker : brokerOrders) {
    OrderRecord* rec = byRemarkOrVenueId(broker.remark, broker.orderId);
    if (rec == nullptr) {
      const auto state = fromFutuStatus(broker.status).value_or(OmsState::kUnknown);
      if (!isTerminal(state)) {
        const OrderRecord& adopted = adopt(broker);
        seen.insert(adopted.clOrdId);
        ++report.adoptedExternal;
      }
      continue;
    }
    seen.insert(rec->clOrdId);
    if (handlePresumedDead(*rec, broker)) {
      report.drifts.push_back({DriftKind::kOrderMissingAtBroker,
                               rec->clOrdId + " was presumed dead but the broker lists it"});
      continue;
    }
    rec->missedListings = 0;
    const bool wasUnknown = rec->state == OmsState::kUnknown;
    const auto observed = fromFutuStatus(broker.status);
    if (observed && *observed == OmsState::kUnknown) {
      report.drifts.push_back({DriftKind::kOrderStatusUnknown,
                               rec->clOrdId + ": broker status " + std::to_string(broker.status)});
    }
    if (observe(*rec, broker)) {
      ++report.statusCatchUps;
      if (wasUnknown) {
        ++report.resolvedUnknown;
      }
    }
  }
  const std::int64_t now = clock.nowNs();
  // Snapshot: the loop below may change states (and so the live index) as it goes.
  std::vector<OrderRecord*> liveNow;
  liveNow.reserve(live.size());
  for (const auto& [liveSeq, recPtr] : live) {
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
      if (now - rec.sentAtNs >= config.ambiguityGraceMs * kNsPerMs &&
          rec.missedListings >= config.absenceListingsRequired) {
        if (rec.state == OmsState::kPendingSubmit) {
          fire(rec, OmsEvent::kSubmitAmbiguous, "not listed after grace");
        }
        fire(rec, OmsEvent::kRejected, "not found at broker after grace period");
        rec.detail = "not found at broker after grace period";
        rec.presumedDead = true;  // if it ever shows up, that is an incident, not a resolution
        ++report.resolvedUnknown;
      }
      continue;
    }
    report.drifts.push_back({DriftKind::kOrderMissingAtBroker,
                             rec.clOrdId + " (" + toString(rec.state) + ") is not at the broker"});
  }
}

// 2) Fills and positions. A fill can land between the two queries, so re-check once before calling
//    a mismatch real.
std::optional<std::string> Oms::Impl::reconcilePositions(ReconcileReport& report) {
  bool positionsOk = false;
  for (int attempt = 0; attempt < 2 && !positionsOk; ++attempt) {
    auto fills = venue.listFills();
    if (!fills) {
      return "fill list: " + fills.error().message;
    }
    auto positions = venue.listPositions();
    if (!positions) {
      return "position list: " + positions.error().message;
    }
    std::scoped_lock lock(mu);
    for (const auto& fill : fills.value()) {
      applyFillLocked(fill, &report.missedFillsApplied);
    }
    std::unordered_map<std::string, std::int64_t> brokerQty;
    for (const auto& pos : positions.value()) {
      brokerQty[pos.code] = pos.qty;
    }
    std::vector<Drift> mismatches;
    std::set<std::string> symbols;
    for (const auto& pos : book.all()) {
      symbols.insert(pos.symbol);
    }
    for (const auto& [code, qty] : brokerQty) {
      symbols.insert(code);
    }
    for (const auto& symbol : symbols) {
      const std::int64_t ours = book.qty(symbol);
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
  return std::nullopt;
}

// 3) Cash (opt-in): broker cash should equal baseline plus what our fills moved.
std::optional<std::string> Oms::Impl::reconcileCash(ReconcileReport& report) {
  if (!config.cashToleranceMills) {
    return std::nullopt;
  }
  const auto funds = venue.funds();
  if (!funds) {
    return "funds: " + funds.error().message;
  }
  std::scoped_lock lock(mu);
  if (baselineCash) {
    const Int128 expected = static_cast<Int128>(*baselineCash) + book.cashDelta();
    const Int128 diff = expected - funds.value().cash;
    if ((diff < 0 ? -diff : diff) > *config.cashToleranceMills) {
      report.drifts.push_back(
          {DriftKind::kCashMismatch, "expected " + std::to_string(saturate(expected)) + " broker " +
                                         std::to_string(funds.value().cash)});
    }
  }
  return std::nullopt;
}

// The verdict: a complete listing resets the failure streak; any drift trips the kill switch.
void Oms::Impl::concludeReconcile(ReconcileReport& report) {
  report.complete = true;
  std::scoped_lock lock(mu);
  failedReconciles = 0;
  log(JournalKind::kReconcile, "",
      "complete: drifts " + std::to_string(report.drifts.size()) + ", missed fills " +
          std::to_string(report.missedFillsApplied) + ", catch-ups " +
          std::to_string(report.statusCatchUps));
  if (!report.drifts.empty()) {
    autoTrip("reconciliation drift: " + report.drifts.front().detail);
    report.halted = true;
  }
}

}  // namespace futu_trader::oms
