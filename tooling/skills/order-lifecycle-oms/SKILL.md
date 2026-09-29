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
