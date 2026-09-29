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
9. **REAL money is unforgeable at compile time.** `opend::AccountHeader` has no public way to build a REAL header; only `oms::TradeTarget` can, and a REAL `TradeTarget` needs a `LiveApproval` that only `LiveGate::approveReal` issues. Never add a public constructor or a `TrdEnv` parameter to a request API. Broker events use `WireHeader`, which cannot address a request.
10. **An ambiguous submit is never a rejection.** Only `kServer`/`kInvalidArg` prove the order does not exist; timeouts, disconnects and unknown OpenD `retType`s leave it `Unknown` until reconciled. An order declared dead by absence stays blocked under its intent key, and its late appearance halts trading.
11. **Every automatic halt must also cancel** resting orders. Push handlers (`Oms::onOrderUpdate/onFill`, `PushRouter`) only set a flag; the engine thread must call `Oms::serviceHalt()` regularly (submit and reconcile also do). Never call the venue from the OpenD push thread: it delivers the venue's own replies.
12. Risk-reducing orders bypass loss and order-count limits (flattening must always be possible); everything that adds exposure fails closed on unset limits.
13. **Check the build's exit code, not a grep of its output.** GCC prints `: error:`; a filter for ` error ` hides failures and you end up testing a stale binary.

## Layout
`include/futu_trader/<module>/`, `src/<module>/`, `tests/{unit,e2e}`, `tools/mock_opend` (test-only fake OpenD with fault injection), `config/*.yaml`, `tooling/{agents,skills}`.
Roadmap (P0..P5) is in the approved plan; status: P0 done; P1 (OpenD connectivity) and P2 (OMS, risk, portfolio, live gate) done and tested against the mock only, NOT yet against a real OpenD. P2 was independently reviewed by the risk-manager and security-engineer agents; their findings are fixed or listed under known gaps.

## Known gaps (be honest about these)
- `LiveGate` is not wired to a `main` yet: nothing reads `FUTU_LIVE_TRADING`/`--live`, loads the promotion log, or runs the engine loop that must call `serviceHalt()`. There is no runnable trading binary yet (P3/P5).
- Promotion log is an unauthenticated text file (guards mistakes and staleness, not a local attacker); the trade-password MD5 has no memory-hygiene handling (no zeroing, `RLIMIT_CORE`); the OpenD link is plaintext (loopback enforced, no RSA/AES).
- Pre-trade chain has no session/auction check, no cash/buying-power check, no drawdown check. Halting cancels orders but does not flatten positions.
- If an order-update push arrives before its fill push, exposure is briefly understated until the fill is applied (reconciliation catches lasting drift).
- CI/Docker supply chain (unpinned images/actions, no signing/SBOM) is untouched (P5).

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
