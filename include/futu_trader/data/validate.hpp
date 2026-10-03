#pragma once

#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/market/quote.hpp"

namespace futu_trader::data {

/**
 * The invariants every market data source must satisfy before it is replayed: a non-empty symbol,
 * bid > 0, ask >= bid, non-negative sizes and last, and timestamps that never go backwards. CSV
 * files and recorded logs both go through this, because a checksum proves the bytes are intact,
 * not that they make sense. The error names the offending event index.
 */
Result<bool> validateQuotes(const std::vector<QuoteEvent>& quotes);

}  // namespace futu_trader::data
