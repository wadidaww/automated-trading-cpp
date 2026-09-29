---
name: low-latency-cpp
description: "Project conventions for hot-path C++: rings, allocation, false sharing, pinning, benchmarking and reading latency histograms."
---

# Low-latency C++ conventions

- Engine thread is single-writer: no locks, no heap allocation after warm-up (assert with a malloc hook in tests), no syscalls except socket IO.
- SPSC rings: power-of-two capacity, `alignas(64)` head/tail, cached opposing index. Never drop order/fill events; conflate quotes over high-water mark.
- Events are fixed-size PODs; strings interned to `SymbolId` at decode; per-symbol state in struct-of-arrays.
- Memory: `mlockall`, pre-fault pages, monotonic arenas/`std::pmr` for scratch; protobuf arena reset per message on the IO thread.
- Threads pinned to isolated cores; journal thread on a housekeeping core. WSL2 numbers are not representative: benchmark on bare metal.
- Measure first: Google Benchmark with repetitions (report median and CV), `benchmark::DoNotOptimize`, HDR histograms driven at fixed rate (avoid coordinated omission). Use perf, `perf c2c`, flame graphs.
- Starting p99 budgets: decode <5us, ring hop <300ns, strategy on-event <5us, engine->socket <20us. Revise after measurement.
- Apply `TCP_NODELAY`, `SO_BUSY_POLL`, hugepages, LTO/PGO only where a benchmark shows benefit.
