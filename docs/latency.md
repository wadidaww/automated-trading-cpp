# Latency engineering (P4)

What was measured, what was changed because of it, what an independent review found wrong with
the first version of this work, and what is **not** achieved. Numbers come from `futu_bench`
(`cmake --preset bench && ./build/bench/futu_bench`); raw outputs are in `docs/bench/`.
Reproduce before trusting; never compare across machines.

## Environment and how much to trust it

AMD Ryzen 9 6900HS (16 threads = 8 cores x 2 SMT), **WSL2** (Linux 5.15 on Hyper-V), GCC 13.3,
`-O3` Release, no core isolation.

* WSL2 is a virtual machine on a desktop host: the host can deschedule a vCPU for milliseconds at
  any time. **Medians (p50) are repeatable here; tails (p99.9, max, and often p99) are host noise**
  and swing by 10-100x between runs of the same binary. Do not quote them as the system's latency.
* The benchmark picks two CPUs on **different physical cores** from
  `/sys/devices/system/cpu/*/topology/thread_siblings_list` and prints them. (The first version
  hard-coded cpu2 and cpu3, which are hyperthread siblings of one core: a flattering, invalid
  "cross-thread" number. Found by review, fixed, re-measured.)
* Tail targets must be verified on bare-metal Linux with isolated cores (`isolcpus`,
  `nohz_full`, IRQ affinity). `EngineConfig` exposes `engineCpu`, `reconcilerCpu`,
  `engineRealtimePriority` and `busyPoll`; they are off by default, best-effort, and the engine
  reports whether pinning took effect.
* There is no network in these numbers. OpenD adds **tens of milliseconds** to any order;
  microsecond work here only avoids adding jitter on top of that. This system is not, and cannot be,
  a sub-millisecond market maker through OpenD.

## Method

`futu_bench` times operations individually (clock overhead, ~37 ns, subtracted) or in batches for
sub-100 ns work, into a log-linear histogram (`infra::LatencyHistogram`, <= ~6% bucket resolution;
percentiles are bucket upper bounds so they never understate). Specifics that matter:

* **Batched rows** (`spsc_push+pop`, `pretrade_risk_check`, `histogram_record`) report percentiles
  *of batch means*, which hide tails. Only the p50 is a meaningful per-call figure for them.
* **Engine rows** come from a real two-thread run: a paced producer on one core, the busy-polling
  engine thread on another, measured by the engine itself. A 4,000-quote warm-up precedes the
  measured phase (first-use paths and cold caches must not pollute the tail). Each measured quote
  is stamped with its **scheduled** arrival time (`Engine::onQuoteAt`), so a stall of the sender
  is counted as latency rather than silently removing the samples that would show it
  (coordinated omission).
* **`oms_submit` rows**: every iteration finishes one more order, so history *grows by one per
  operation*. A row's label gives the history it starts at, not a fixed size.
* The "quote -> order -> fill" engine row uses an `InstantVenue` whose hook delivers the fill and
  order update **re-entrantly inside `place()`**. It measures our own processing with a broker that
  answers in zero time. It is not a realistic broker, and excludes any network time.
* Google Benchmark is not used (not installed here); the harness is ~600 lines in `bench/`.

## Budget versus measured (nanoseconds, Release, final `after-optimization.txt`)

| Stage | Plan budget (p99) | p50 | p99 | Verdict |
|---|---|---|---|---|
| OpenD frame decode (header + SHA-1 + reassembly) | < 5,000 | 218 | 458 | met |
| SPSC ring hop, cross-thread one-way, 64-byte tick, distinct cores | < 300 | 127 | 167 | met (p50 119-127 over 5 runs) |
| Pre-trade risk check, accept path | | 33 | 75* | met |
| Strategy on quote, reference (no order) | < 5,000 | 8 | 46 | met |
| Engine handle, quote -> decision | < 5,000 | 210 | 794 | met |
| Engine handle, quote -> order -> fill processed (instant broker) | < 5,000 | 7,130 | 26,586 | **p50 and p99 over budget** |
| OMS submit alone, instant broker | < 5,000 | 2,300 | ~11,000-14,000 | p50 met, p99 not met |
| Engine thread -> socket write | < 20,000 | not measured | | no real socket in the bench |
| Ring queue wait | | ~220 | 60,000 - 800,000 | **tail is host noise; unverifiable here** |

\* percentile of batch means. The order path is limited by the exchange to ~15 orders per 30 s per
account, so its p99 does not constrain throughput; it only affects decision-to-order latency,
which OpenD's tens of milliseconds dwarf.

## What the measurements found, and what changed

Baseline first, then fix what the profile (not intuition) pointed at:

| Finding | Before | After |
|---|---|---|
| **The OMS scanned every order ever placed** on each submit and on every `hasLiveOrder` (called once per quote). 95% of profile time. | submit p50 **3.9 ms** at 20k orders of history; 86 us at the start of a run; grew with history | **~2.7 us** at 20k history; roughly 1,000-1,500x faster at that history, and ~1.3x growth from 0 to 20k (cache effects) instead of linear |
| Same flaw in the simulated broker (`SimVenue::onQuote` and reservations walked all orders) | backtest 26 us/event | **~4 us/event, ~250k events/s** (6x vs baseline) |
| Hand-written SHA-1 was >90% of frame decode (and OpenSSL's one-shot `SHA1()` was *also* slow: a provider lookup per call) | decode 1,498 ns | **218 ns** (SHA-1 146 ns, ~10x) via the low-level `SHA1_*` API, checked against the portable code by a differential test |
| Mean-reversion signal rescanned its window every quote | 98 ns | **8-10 ns** (exact integer running sums, ring buffer so it never allocates) |
| Hash maps / vectors growing at power-of-two sizes stalled the engine thread | worst-case handling ~5 ms | ~0.2-0.6 ms (`OmsConfig::reserveOrders` pre-sizes them) |
| In-memory journal grew without bound | unbounded | bounded (`journalRetain`); durable history belongs in the WAL |

Measured and **rejected**: link-time optimisation was 1.00x on almost everything and slightly
slower on the backtest, so it is not enabled. A regression I introduced and then caught by
re-measuring: replacing `std::deque` with a ring buffer using `%` cost ~20 ns until the modulo
became a compare-and-wrap.

## What the independent review found (all fixed unless listed under "not achieved")

* **False sharing on every quote.** Producer-written and consumer-written counters shared cache
  lines, and the producer read the consumer's index after every push. Counters are now grouped by
  writer on separate lines, the backlog high-water mark is sampled by the consumer once per poll,
  and a quote is exactly one aligned 64-byte cache line so adjacent ring slots never share one.
* **Benchmark methodology** (SMT siblings; no warm-up; coordinated omission; batched rows labelled as
  if per-call; mislabelled history rows; "flat" claim contradicted by the data): all corrected above.
* **Stale backlog.** After a stall the strategy would have traded on thousands of old quotes.
  Quotes that waited longer than `maxQuoteAgeNs` (default 1 s) are now skipped and counted; a full
  ring still drops the newest quote and counts it. Both counters should alarm.
* **Exception safety.** Only the strategy call was guarded. Now every engine-thread path is, a
  failing halt falls back to tripping the kill switch, and an unexpected exception sets
  `engineFailed` instead of terminating with orders working.
* **Concurrency.** `start()`/`stop()` are serialised (overlapping calls terminated the process);
  quotes offered after `stop()` are refused and counted; debug builds assert the single-producer /
  single-consumer rule; `SpscRing::sizeApprox()` is clamped (it could wrap or exceed capacity when read
  from a third thread, found by a new test); a halt requested externally no longer re-sends cancels
  that another thread just sent.
* **Misstated accounting identity.** It is
  `received == processed + staleSkipped + dropped + badSymbol + rejectedAfterStop` and holds only
  when the ring is empty and no call is in flight.

## Enforced by tests (not just measured)

* `tests/alloc/alloc_test.cpp` (own executable; counts allocations on the **calling thread** with
  a replaced `operator new`; built in the Debug `dev` preset only, never under sanitizers): for a
  **single symbol in steady state**, the quote path (market data in -> strategy decision, no order)
  performs **zero heap allocations** across 5,000 quotes; ring, histogram and risk check are
  allocation-free; an accepted **order costs ~14 allocations** (capped at 40). A negative control
  proves the counter can see allocations. It does not cover first-seen symbols, the engine's own
  thread, or the order-placing branch of a strategy.
* `SpscRing`: two-thread test delivering 400k items in order with no loss or duplication (TSan in
  CI), `sizeApprox` bounds, cache-line layout; `LatencyHistogram`: accuracy, overflow, concurrency.
* Engine: accounting, ordering, staleness, drain-on-stop, restart, concurrent start/stop,
  strategy faults (standard and non-standard exceptions), kill-flag file, reconciler thread,
  push-thread halt, counter cache-line separation, all under TSan (80 consecutive runs clean).

## Not achieved (be honest)

* **The engine thread blocks on the broker.** `strategy -> OMS -> venue.place()` is a synchronous
  network call (tens of ms through OpenD), as is a cancel-all halt. While it blocks, quotes queue
  and, past `maxQuoteAgeNs`, are discarded. That is acceptable for the intraday strategies targeted
  here and buys a simple deterministic order lifecycle, but it means **order handling is not
  sub-millisecond and stalls quote processing**. A dedicated order-sender thread with an
  asynchronous submit API would fix it and has not been built.
* **Not lock-free on the engine thread.** Each quote takes ~3 uncontended mutexes (OMS mark,
  position book, live-order check, ~20 ns each). They can be contended by the reconciler and push
  threads; hold times are short and never span a network call.
* **Zero allocation holds for the quote path only** (and only as scoped above). Order handling
  allocates and is not optimised, because the exchange rate limit bounds it.
* **Tail latency is unverified** (WSL2). No bare-metal run exists; isolated cores, huge pages,
  `mlockall`, real-time priority and busy-poll tuning are provided but unvalidated.
* `Oms::records` (every order of the session) is never pruned: ~hundreds of bytes per order, fine
  for a day, but unbounded across a long-running process. Only the journal is capped.
* Protobuf decode uses the default allocator (~380-730 ns for a small push, noisy); an arena would
  cut it, but the OpenD push decoders are not wired into the engine yet.
* The ~1M events/s backtest goal is not met (~250k/s with full OMS, risk and reconciliation).
* End-to-end tick-to-wire against a real OpenD has never been measured. CI's `bench-smoke` job only
  reports numbers; there is no regression gate (shared runners are too noisy to gate on).

## Reproducing

```
cmake --preset bench && cmake --build --preset bench
./build/bench/futu_bench                 # full table (prints the CPU pair it pinned to)
./build/bench/futu_bench --filter oms    # groups: framing ring oms strategy engine histogram backtest
./build/bench/futu_bench --json out.json --quick
```
`docs/bench/before-optimization.txt` is the pre-optimisation baseline (its ring/engine rows were
measured on SMT siblings and are not comparable); `after-optimization.txt` is the final run.
