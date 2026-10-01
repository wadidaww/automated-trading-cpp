#include "futu_trader/data/validate.hpp"

namespace futu_trader::data {

Result<bool> validateQuotes(const std::vector<QuoteEvent>& quotes) {
  std::int64_t previous = INT64_MIN;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const QuoteEvent& q = quotes[i];
    const auto bad = [&](const std::string& why) {
      return Error{ErrorCode::kInvalidArg, "market data event " + std::to_string(i) + ": " + why};
    };
    if (q.symbol.empty()) {
      return bad("empty symbol");
    }
    if (q.bid <= 0 || q.ask < q.bid) {
      return bad("bid must be > 0 and <= ask");
    }
    if (q.last < 0 || q.bidSize < 0 || q.askSize < 0) {
      return bad("negative last price or size");
    }
    if (q.tsNs < 0 || q.tsNs < previous) {
      return bad("timestamps go backwards or are negative");
    }
    previous = q.tsNs;
  }
  return true;
}

}  // namespace futu_trader::data
