#pragma once

#include <string>
#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/market/quote.hpp"

namespace futu_trader::data {

/**
 * Parses a decimal price like "350.2" into mills exactly (no floating point). At most 3 decimals;
 * no sign, exponent or spaces; rejects empty input and overflow.
 */
Result<Money> parsePriceMills(const std::string& text);

/**
 * Parses quote CSV: header `ts_ms,symbol,bid,ask,last,bid_size,ask_size`, one quote per row.
 * Timestamps are UTC epoch milliseconds and must not go backwards; bid must be > 0 and <= ask.
 * Any bad row fails the whole file with its line number: a silently skipped row is a data hole.
 */
Result<std::vector<QuoteEvent>> parseQuotesCsv(const std::string& text);

}  // namespace futu_trader::data
