# futu-quant-trader

Production-oriented C++20 algorithmic trading system scaffold for Futu OpenAPI (OpenD), with modular architecture for data ingestion, signal modeling, risk checks, execution, and evaluation.

## Architecture
See [docs/architecture.md](docs/architecture.md). In short: OpenD pushes quotes into a lock-free ring, one
engine thread runs the strategy, every order goes through the OMS (pre-trade risk, rate limit,
write-ahead log, kill switch) to an `IVenue`, and a reconciler keeps our books equal to the broker's.
Backtests run the same OMS against a simulated broker.

## Prerequisites
- CMake 3.25+, Ninja, a C++20 compiler
- libprotobuf, Boost (headers), OpenSSL, yaml-cpp, GoogleTest (or vcpkg)
- Futu OpenD gateway (paper or live account) to run the trader

## Build and test
```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
```
(`ci` = ASan+UBSan with `-Werror`, `tsan` = ThreadSanitizer.)

## Run (SIMULATE by default)
```bash
./build/dev/futu_trader --config config/paper.yaml
```
REAL money needs the full live gate: read `docs/runbook.md` first.

## Configuration
- `config/paper.yaml`: SIMULATE trading. Every risk limit is mandatory; unknown keys are errors.
- `config/live.example.yaml`: REAL trading template (deliberately small limits).
- `config/config.{dev,staging,prod}.yaml`: only for the offline `futu_model_train` tool.

## Backtesting
```bash
./scripts/run_backtest.sh
```

## Model training
```bash
cmake --preset dev -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build --preset dev -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
./build/dev/futu_model_train --config config/config.dev.yaml
```

## Production deployment
Use `.github/workflows/deploy-prod.yml` (manual dispatch + environment approval).

## Risk disclaimer
Trading involves substantial risk. This software is provided for research and engineering purposes only.
