#include "futu_trader/data/csv.hpp"

#include <sstream>

namespace futu_trader::data {

namespace {

__extension__ using Int128 = __int128;

bool parseUint(const std::string& text, std::int64_t& out) {
  if (text.empty() || text.size() > 18) {
    return false;
  }
  std::int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = (value * 10) + (c - '0');
  }
  out = value;
  return true;
}

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream stream(line);
  while (std::getline(stream, field, ',')) {
    fields.push_back(field);
  }
  if (!line.empty() && line.back() == ',') {
    fields.emplace_back();
  }
  return fields;
}

}  // namespace

Result<Money> parsePriceMills(const std::string& text) {
  const auto dot = text.find('.');
  const std::string whole = text.substr(0, dot);
  std::string frac = dot == std::string::npos ? "" : text.substr(dot + 1);
  std::int64_t wholePart = 0;
  if (!parseUint(whole, wholePart) || (dot != std::string::npos && frac.empty()) ||
      frac.size() > 3) {
    return Error{ErrorCode::kInvalidArg, "bad price '" + text + "'"};
  }
  while (frac.size() < 3) {
    frac += '0';
  }
  std::int64_t fracPart = 0;
  if (!parseUint(frac, fracPart)) {
    return Error{ErrorCode::kInvalidArg, "bad price '" + text + "'"};
  }
  const Int128 mills = (static_cast<Int128>(wholePart) * 1000) + fracPart;
  if (mills > INT64_MAX / 1000) {
    return Error{ErrorCode::kInvalidArg, "price out of range '" + text + "'"};
  }
  return static_cast<Money>(mills);
}

Result<std::vector<QuoteEvent>> parseQuotesCsv(const std::string& text) {
  std::istringstream lines(text);
  std::string line;
  int lineNo = 0;
  std::vector<QuoteEvent> out;
  bool sawHeader = false;
  std::int64_t previousTs = -1;
  const auto bad = [&](const std::string& why) {
    return Error{ErrorCode::kInvalidArg, "line " + std::to_string(lineNo) + ": " + why};
  };
  while (std::getline(lines, line)) {
    ++lineNo;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    if (!sawHeader) {
      if (line != "ts_ms,symbol,bid,ask,last,bid_size,ask_size") {
        return bad("unexpected header");
      }
      sawHeader = true;
      continue;
    }
    const auto fields = split(line);
    if (fields.size() != 7) {
      return bad("expected 7 fields");
    }
    std::int64_t tsMs = 0;
    std::int64_t bidSize = 0;
    std::int64_t askSize = 0;
    if (!parseUint(fields[0], tsMs) || !parseUint(fields[5], bidSize) ||
        !parseUint(fields[6], askSize) || fields[1].empty() || fields[1].size() > 32) {
      return bad("bad timestamp, symbol or size");
    }
    const auto bid = parsePriceMills(fields[2]);
    const auto ask = parsePriceMills(fields[3]);
    const auto last = parsePriceMills(fields[4]);
    if (!bid || !ask || !last) {
      return bad("bad price");
    }
    if (bid.value() <= 0 || ask.value() < bid.value()) {
      return bad("bid must be > 0 and <= ask");
    }
    if (tsMs < previousTs) {
      return bad("timestamps go backwards");
    }
    if (tsMs > INT64_MAX / 1'000'000) {
      return bad("timestamp out of range");
    }
    previousTs = tsMs;
    QuoteEvent quote;
    quote.tsNs = tsMs * 1'000'000;
    quote.symbol = fields[1];
    quote.bid = bid.value();
    quote.ask = ask.value();
    quote.last = last.value();
    quote.bidSize = bidSize;
    quote.askSize = askSize;
    out.push_back(std::move(quote));
  }
  if (!sawHeader) {
    return bad("empty file");
  }
  return out;
}

}  // namespace futu_trader::data
