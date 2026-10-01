#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <vector>

#include "futu_trader/market/quote.hpp"

namespace futu_trader::data {

/**
 * Compact, versioned, checksummed binary log of market events. The live recorder writes it and
 * the backtester/replayer reads it, so a recorded session can be replayed exactly.
 *
 * Layout (little-endian): header "FTEL" u16 version u16 reserved; then records
 *   u32 payloadLen | payload | u32 crc32(payload)
 * where payload = u8 kind (1 = quote) + fields. A torn tail (crash mid-write) is detected as
 * kTruncated and every complete record before it is still returned.
 */
inline constexpr std::uint16_t kEventLogVersion = 1;
inline constexpr std::size_t kMaxLogRecordBytes = 1U << 20;

std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

class EventLogWriter {
 public:
  /** Writes the header immediately. */
  explicit EventLogWriter(std::ostream& out);
  /** Returns false (and stops writing) if the symbol is too long or the stream failed. */
  bool writeQuote(const QuoteEvent& quote);
  bool ok() const { return ok_; }

 private:
  std::ostream& out_;
  bool ok_{true};
};

enum class LogStatus : std::uint8_t {
  kOk,
  kTruncated,    // ends mid-record: everything before it was read
  kBadChecksum,  // a record's CRC does not match
  kBadHeader,    // not an event log
  kBadVersion,   // a version this build does not understand
  kBadRecord,    // structurally invalid record (unknown kind, oversize, bad fields)
};

struct LogReadResult {
  std::vector<QuoteEvent> quotes;  // every record read successfully, in order
  LogStatus status{LogStatus::kOk};
};

/** Reads until end of stream or the first problem. Never trusts a length field blindly. */
LogReadResult readEventLog(std::istream& in);

}  // namespace futu_trader::data
