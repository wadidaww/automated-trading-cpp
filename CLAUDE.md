# CLAUDE.md

C++20 automated trading system for Futu/moomoo OpenD (HK equities first). Goal: production-grade,
low-latency, deterministic. **Read this before changing order flow, risk, or the OpenD layer.**

## Build & test
```
cmake --preset dev && cmake --build --preset dev && ctest --preset dev   # Debug
cmake --preset ci  && cmake --build --preset ci  && ctest --preset ci    # ASan+UBSan, -Werror
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan # ThreadSanitizer
```
- Tests are GoogleTest (`tests/unit`, `tests/e2e`). Never use bare `assert` in tests (it vanishes under NDEBUG).
- Format with `clang-format -i`; lint with `clang-tidy -p build/dev <file>`. CI blocks on both and on `-Werror`.
- Concurrency changes must pass `ctest --preset tsan --repeat until-fail:30`.
- vcpkg is optional (`VCPKG_ROOT`); otherwise system packages are used (needs libgtest-dev, libprotobuf-dev, protobuf-compiler, libboost-dev).
- Asio sockets must never be `close()`d while another thread is inside an operation on them: `shutdown()` from other threads, `close()` only after joining (TSan enforces this).

## Rules that must not be broken
1. **Money is int64 minor units.** No `double` for cash, PnL accounting or price comparison. Overflow-check arithmetic.
2. **Default is SIMULATE.** `TrdEnv::kReal` requires the full live gate (config phrase + `FUTU_LIVE_TRADING` env + `--live` + promotion record). See `tooling/skills/trading-risk-checklist`.
3. **Fail closed.** An unset risk limit means reject, not unlimited.
4. **Check the venue result.** Never record an order as submitted unless the venue accepted it.
5. **No network calls or blocking IO while holding a mutex.**
6. **OpenD wire layer lives in `src/opend/`** (framing, connection, client), with protos vendored in `third_party/futu_proto` (pinned, see PROTO_VERSION). Command IDs are NOT in the `.proto` files: `include/futu_trader/opend/proto_ids.hpp` pins them to the SDK's table and a test asserts every value. Never hand-type an ID elsewhere. The old `FutuClient` (`api/futu_client.hpp`) is a legacy in-memory stub with WRONG ids (place/cancel/getKl/snapshot); do not use it for anything real. It goes away in P2.
7. **Determinism:** strategy code reads time only from an injected clock; no `random_device`; no iteration over unordered containers in the decision path.
8. Do not use `sleep_for` to wait in tests; use `drain()` or drive a clock.

## Layout
`include/futu_trader/<module>/`, `src/<module>/`, `tests/{unit,e2e}`, `tools/mock_opend` (test-only fake OpenD with fault injection), `config/*.yaml`, `tooling/{agents,skills}`.
Roadmap (P0..P5) is in the approved plan; status: P0 done; P1 (read-only OpenD connectivity) done and tested against the mock only, NOT yet against a real OpenD.

## Known scaffold gaps (not yet real)
`FutuClient` (legacy) is an in-memory stub; the real `opend::OpenDClient` has no order placement yet (P2); `PositionTracker`/`ModelRegistry`/`DataStore` are unwired;
`scripts/run_backtest.sh` writes a hard-coded report (its CI gate is meaningless until P3);
`scripts/setup_opend.sh`, `deploy-prod.yml` and the `mock-opend` compose service are placeholders.

## Agents & skills (tracked in `tooling/`, symlinked into `.claude/` locally)
Agents: quantitative-developer, execution-trader, risk-manager (veto on order-flow/live-gating changes),
backtest-validator, opend-integration-engineer, market-data-engineer, sre-trading-ops; plus the general
low-latency-engineer / security-engineer. Skills: futu-opend-integration, trading-risk-checklist,
backtest-integrity, low-latency-cpp, deterministic-simulation-testing, order-lifecycle-oms, hk-market-rules.
To load them in a fresh clone: `for f in tooling/agents/*.md; do ln -sf ../../$f .claude/agents/; done` and
`for d in tooling/skills/*; do ln -sfn ../../$d .claude/skills/; done`.

## Honest platform limits
OpenD is a local gateway: tens of ms to the venue, ~15 orders/30 s, conflated pushes. Target is deterministic
intraday systematic trading, not sub-millisecond HFT. Benchmarks on WSL2 are not representative.
