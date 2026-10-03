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


## Lessons from the P3 review (src/backtest/*)
- **Costs:** report closed-trade P&L NET of fees (PositionBook realized P&L is gross). A 67% gross hit rate became 21% net for a strategy that lost 2%.
- **Equity:** mid-marked final equity flatters open positions; also report liquidation value (touch price minus exit costs).
- **Resampling:** never carry equity forward across empty periods (overnight/weekend): it invents zero-return periods. Derive `periodsPerYear` from the sampling period, never set it independently.
- **Reliability:** flag ratios from fewer than ~250 periods as unreliable, and always print a bootstrap CI; a CI that could not be computed must say so (`valid == false`), not read `{0,0}`.
- **Liquidity:** displayed size is consumed by our own fills; an identical refreshed quote is not new liquidity; unknown size must not mean unlimited in real runs.
- **Lookahead detector:** cut right after sampled decisions and include the LAST one; hand the factory the dataset of each run (whole-series precomputation is the usual cheat); zero cuts tested is a failure; it must be part of the gate, and the cheating canary must be required to fail.
- **Gate integrity:** prove a gate can fail (tamper a golden, expect exit 4) and that "nothing happened" (no trades, wrong symbol) is an error, not a pass.
