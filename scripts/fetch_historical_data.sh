#!/usr/bin/env bash
# Historical data download is NOT implemented yet.
#
# The backtester reads quote CSV (`futu_backtest --csv FILE`, format
# ts_ms,symbol,bid,ask,last,bid_size,ask_size) or a recorded event log (`--log FILE`). Producing
# them from a real OpenD (history KL and live recording) is planned work; until then this script
# fails loudly rather than writing an empty file that looks like data.
echo "fetch_historical_data.sh: not implemented (see CLAUDE.md, 'Known gaps')" >&2
exit 1
