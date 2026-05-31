#!/usr/bin/env bash
set -euo pipefail
mkdir -p data/raw
echo "timestamp,symbol,open,high,low,close,volume" > data/raw/sample_ohlcv.csv
echo "Fetched fixture historical data to data/raw/sample_ohlcv.csv"
