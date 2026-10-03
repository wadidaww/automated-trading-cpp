---
name: deterministic-simulation-testing
description: "How to write seeded, fault-injected simulation scenarios and invariants for the trading Engine."
---

# Deterministic simulation testing

- Compose the Engine with `SimClock`, `ReplayMarketData`, `SimVenue` and a seeded RNG owned by the context.
- Schedule faults from the seed: disconnects, delayed/duplicated/out-of-order acks and fills, rate-limit rejects, stale quotes, halts.
- Run thousands of seeds nightly; on failure print the seed and reproduce locally.
- Invariants: no order after halt; risk limits never breached; OMS state equals SimVenue truth at end of day; no double submission for one ClOrdId; cash + positions conserved except fees; journals byte-identical across reruns of the same seed.
- Forbidden in decision path: wall clock, `random_device`, unordered-container iteration, thread timing dependence. Tests must not use `sleep_for` to wait; drive the clock.
