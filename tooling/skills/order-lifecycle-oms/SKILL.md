---
name: order-lifecycle-oms
description: "Order state machine, legal transitions, idempotency and reconciliation patterns."
---

# Order lifecycle

States: New -> PendingSubmit -> Working -> PartiallyFilled -> Filled | Cancelled | Rejected; plus Unknown (ambiguous after timeout/disconnect).

- Only legal transitions apply; every transition is journaled. Duplicate/unknown broker events are absorbed, never crash.
- `ClOrdId` = session epoch + journal counter, sent in `remark`. After a timeout, look up by remark in the order list before any resend. `Unknown` blocks resubmission of that intent until reconciled.
- Never record an order as Submitted until the venue accepted it; check the placeOrder result.
- Reconcile on startup, reconnect and periodically: compare OMS vs broker orders, fills, positions, funds. Broker is truth for state, journal is truth for intent. Auto-heal only known patterns (missed push where the fill is in the fill list); halt otherwise.
- Property-test the FSM with random legal/illegal sequences.


## Rules learned from review (see src/oms/oms.cpp)
- **Ambiguity ladder:** `kServer`/`kInvalidArg` = definitely refused. `kTimeout`/`kDisconnected`/`kProtocol` = ambiguous, including OpenD's own timeout/disconnect/unknown `retType`s. Ambiguous -> `Unknown` (counts as live for risk, blocks the intent and the symbol).
- **Declaring "never existed":** needs the grace period AND N consecutive complete listings without the order. The record becomes `presumedDead`: the intent key stays blocked, and if the broker ever lists it as live/filled the OMS halts, adopts it as `LATE-<id>` (so it is counted and cancelled), and alerts.
- **Cancels are retried:** an unconfirmed cancel is resent after `cancelRetryMs`; halt always resends; a broker still showing "working" past the window sends the order back to Working so it can be cancelled again.
- **Halts finish the job:** every auto-trip sets `haltRequested`; `serviceHalt()` (engine thread only) keeps cancelling until nothing live remains, including after the rate window frees up.
- **Restart:** `bootstrap()` seeds broker positions AND marks today's fills as already applied, otherwise the first reconcile double-counts them. Broker orders carrying our `FT-` remark are recovered as ours.
- **Risk view:** resting same-side orders count as exposure; resting sells reduce sellable shares; a held symbol with a stale mark blocks new exposure (not flattening); freshness uses the OMS clock, never the caller's claim.
