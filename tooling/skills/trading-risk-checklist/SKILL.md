---
name: trading-risk-checklist
description: "Pre-trade risk checklist, live-gating rules and go-live checklist for review of anything touching order flow."
---

# Trading risk checklist

## Pre-trade (all O(1), before any order leaves; each reject has a reason code + metric)
- [ ] Kill/halt state; trading session (no closing-auction orders unless allowed)
- [ ] Symbol allow-list; side and short-sale permission; lot; tick; price band vs last/mid
- [ ] Stale-quote check (reject if older than T ms)
- [ ] Max order qty/notional; max position per symbol; gross/net notional; concentration
- [ ] Open orders total and per symbol; order rate below OpenD limit with headroom
- [ ] Cash/buying power incl. pending orders
- [ ] Daily loss (realized+unrealized), drawdown, per-strategy loss
- [x] Self-trade prevention, duplicate suppression, fat-finger (implemented)
- [ ] Session/auction check, cash/buying power, drawdown (NOT implemented yet)
- [ ] Overflow-safe arithmetic; unset limit = reject

## Live gating
- [ ] Default SIMULATE; REAL needs config phrase + `FUTU_LIVE_TRADING` + `--live` + promotion record
- [ ] Account from GetAccList matches config; `TrdEnv` on every order equals configured env
- [ ] Unlock ok; funds/positions/orders reconciled before first order

## Go-live
- [ ] 5 consecutive SIMULATE days with zero unreconciled discrepancies
- [ ] Kill switch drill passed; kill -9 restart drill passed
- [ ] Alerts fire in staged failure test; no secrets in logs
- [ ] Prod limits reviewed by CODEOWNERS

## Reviewer hints (from the P2 review)
- Every reject path that trips a halt must also cancel resting orders.
- A reduce-only order must pass even after a loss/order-count breach.
- "Unset" limits (0) must reject, never mean unlimited: check `concentrationLimit`, daily loss, notional caps.
- Live gate inputs need `today` from a trusted clock; a stale or invented promotion log must not pass.
