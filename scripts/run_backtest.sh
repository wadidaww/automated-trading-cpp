#!/usr/bin/env bash
# Runs the deterministic backtest regression gate.
#
# Replays fixed synthetic market data through the REAL OMS / risk chain / position book against
# the simulated broker, twice (determinism), and compares the result with the committed golden
# reports. It fails if the run is non-deterministic, if the OMS disagrees with the broker at the
# end, or if ANY result changes. An intentional behaviour change means regenerating the golden
# files with `futu_backtest --update-golden <file>` and reviewing the diff in the PR.
#
# It also runs the lookahead detector (an honest strategy must pass) and proves the detector has
# teeth by requiring the deliberately cheating `peeker` canary to be REJECTED (exit code 7).
#
# Note: this is a regression/consistency gate, not evidence of a profitable strategy. The data is
# synthetic and the strategies are reference implementations.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset dev
cmake --build --preset dev --target futu_backtest
mkdir -p data/processed

BT=./build/dev/futu_backtest
"$BT" --synthetic --seed 42 --count 20000 --strategy meanrev --check-determinism \
      --golden tests/golden/backtest_meanrev_seed42.json \
      --report data/processed/backtest_meanrev_seed42.json > /dev/null
"$BT" --synthetic --seed 7 --count 20000 --strategy random --check-determinism \
      --golden tests/golden/backtest_random_seed7.json \
      --report data/processed/backtest_random_seed7.json > /dev/null
"$BT" --synthetic --seed 11 --count 20000 --size 200 --strategy maker --max-daily-loss 900000 \
      --check-determinism --golden tests/golden/backtest_maker_seed11.json \
      --report data/processed/backtest_maker_seed11.json > /dev/null

# Honest strategy: no lookahead. Cheating canary: must be caught.
"$BT" --synthetic --seed 5 --count 3000 --strategy meanrev --check-lookahead > /dev/null
set +e
"$BT" --synthetic --seed 5 --count 3000 --strategy peeker --check-lookahead > /dev/null 2>&1
peeker_rc=$?
set -e
if [ "$peeker_rc" -ne 7 ]; then
  echo "FAIL: the lookahead detector did not reject the cheating canary (exit $peeker_rc)" >&2
  exit 1
fi
echo "Backtest regression gate passed (deterministic, reconciled, matches golden reports)"
