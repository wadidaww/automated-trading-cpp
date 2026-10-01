#include "futu_trader/data/event_log.hpp"

#include <array>
#include <istream>
#include <ostream>

namespace futu_trader::data {

namespace {

constexpr std::uint8_t kKindQuote = 1;
constexpr std::size_t kMaxSymbolBytes = 255;

const std::array<std::uint32_t, 256>& crcTable() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) != 0 ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
      }
      t[i] = c;
    }
    return t;
  }();
  return table;
}

void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v));
  out.push_back(static_cast<std::uint8_t>(v >> 8U));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
  }
}
void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>(v >> (8U * i)));
  }
}

std::uint32_t get32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
         (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}
std::uint64_t get64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(p[i]) << (8U * i);
  }
  return v;
}

bool readExact(std::istream& in, std::uint8_t* dst, std::size_t n) {
  in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
  return static_cast<std::size_t>(in.gcount()) == n;
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  const auto& table = crcTable();
  std::uint32_t c = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    c = table[(c ^ data[i]) & 0xFFU] ^ (c >> 8U);
  }
  return c ^ 0xFFFFFFFFU;
}

EventLogWriter::EventLogWriter(std::ostream& out) : out_(out) {
  std::vector<std::uint8_t> header{'F', 'T', 'E', 'L'};
  put16(header, kEventLogVersion);
  put16(header, 0);
  out_.write(reinterpret_cast<const char*>(header.data()),
             static_cast<std::streamsize>(header.size()));
  ok_ = static_cast<bool>(out_);
}

bool EventLogWriter::writeQuote(const QuoteEvent& quote) {
  if (!ok_ || quote.symbol.size() > kMaxSymbolBytes) {
    return false;
  }
  std::vector<std::uint8_t> payload;
  payload.push_back(kKindQuote);
  put64(payload, static_cast<std::uint64_t>(quote.tsNs));
  payload.push_back(static_cast<std::uint8_t>(quote.symbol.size()));
  payload.insert(payload.end(), quote.symbol.begin(), quote.symbol.end());
  put64(payload, static_cast<std::uint64_t>(quote.bid));
  put64(payload, static_cast<std::uint64_t>(quote.ask));
  put64(payload, static_cast<std::uint64_t>(quote.last));
  put64(payload, static_cast<std::uint64_t>(quote.bidSize));
  put64(payload, static_cast<std::uint64_t>(quote.askSize));

  std::vector<std::uint8_t> record;
  put32(record, static_cast<std::uint32_t>(payload.size()));
  record.insert(record.end(), payload.begin(), payload.end());
  put32(record, crc32(payload.data(), payload.size()));
  out_.write(reinterpret_cast<const char*>(record.data()),
             static_cast<std::streamsize>(record.size()));
  ok_ = static_cast<bool>(out_);
  return ok_;
}

LogReadResult readEventLog(std::istream& in) {
  LogReadResult result;
  std::array<std::uint8_t, 8> header{};
  if (!readExact(in, header.data(), header.size())) {
    result.status = LogStatus::kBadHeader;
    return result;
  }
  if (header[0] != 'F' || header[1] != 'T' || header[2] != 'E' || header[3] != 'L') {
    result.status = LogStatus::kBadHeader;
    return result;
  }
  const auto version = static_cast<std::uint16_t>(header[4] | (header[5] << 8U));
  if (version != kEventLogVersion) {
    result.status = LogStatus::kBadVersion;
    return result;
  }

  while (true) {
    std::array<std::uint8_t, 4> lenBytes{};
    in.read(reinterpret_cast<char*>(lenBytes.data()), 4);
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got == 0) {
      return result;  // clean end of log
    }
    if (got != 4) {
      result.status = LogStatus::kTruncated;
      return result;
    }
    const std::uint32_t len = get32(lenBytes.data());
    // A corrupt length must not make us allocate gigabytes or read past the record.
    if (len == 0 || len > kMaxLogRecordBytes) {
      result.status = LogStatus::kBadRecord;
      return result;
    }
    std::vector<std::uint8_t> payload(len);
    std::array<std::uint8_t, 4> crcBytes{};
    if (!readExact(in, payload.data(), len) || !readExact(in, crcBytes.data(), 4)) {
      result.status = LogStatus::kTruncated;
      return result;
    }
    if (crc32(payload.data(), payload.size()) != get32(crcBytes.data())) {
      result.status = LogStatus::kBadChecksum;
      return result;
    }
    if (payload[0] != kKindQuote) {
      result.status = LogStatus::kBadRecord;
      return result;
    }
    // ts(8) + symLen(1) + sym + 5*8
    if (payload.size() < 1 + 8 + 1) {
      result.status = LogStatus::kBadRecord;
      return result;
    }
    const std::size_t symLen = payload[9];
    if (payload.size() != 1 + 8 + 1 + symLen + 40) {
      result.status = LogStatus::kBadRecord;
      return result;
    }
    QuoteEvent quote;
    quote.tsNs = static_cast<std::int64_t>(get64(&payload[1]));
    quote.symbol.assign(payload.begin() + 10,
                        payload.begin() + 10 + static_cast<std::ptrdiff_t>(symLen));
    const std::uint8_t* fields = &payload[10 + symLen];
    quote.bid = static_cast<Money>(get64(fields));
    quote.ask = static_cast<Money>(get64(fields + 8));
    quote.last = static_cast<Money>(get64(fields + 16));
    quote.bidSize = static_cast<std::int64_t>(get64(fields + 24));
    quote.askSize = static_cast<std::int64_t>(get64(fields + 32));
    result.quotes.push_back(std::move(quote));
  }
}

}  // namespace futu_trader::data
