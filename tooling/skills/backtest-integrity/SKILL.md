---
name: backtest-integrity
description: "Checklist for backtest bias, realism and metric correctness, plus the canary strategy suite."
---

# Backtest integrity

1. **Lookahead**: events with `ts <= now` only; modeled submit/ack latency; explicit bar-close vs next-open.
2. **Universe**: point-in-time; no survivorship; adjustment without future info.
3. **Costs**: commission, platform fee, SFC levy, trading fee, stamp duty; slippage from book sweep; queue position for passive orders.
4. **Constraints**: ticks, board lots, halts, sessions, order-rate limit, short-sale rules.
5. **Metrics**: annualize from the trading calendar; Sharpe/Sortino/Calmar; drawdown depth+duration; turnover; bootstrap CIs; deflated Sharpe for sweeps. Known-answer tests vs numpy.
6. **Determinism**: same data+config+seed => byte-identical journal.
7. **Parity**: replay a recorded SIMULATE session; decisions must match exactly.
8. **Canaries**: buy-and-hold ~ index net of fees; future-peeker must fail the guard; random zero-edge loses ~fees.
9. **Overfitting**: walk-forward, purged/embargoed CV, count of tried parameter sets.
