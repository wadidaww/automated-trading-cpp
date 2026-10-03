#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/infra/histogram.hpp"

namespace futu_trader::infra {

/**
 * Write-ahead log: an append-only file of checksummed records, written by ONE background thread so
 * records from many threads stay in a single total order and callers never touch the disk.
 *
 * Two ways to append:
 *  - appendDurable(): blocks until this record (and everything before it) has been written and
 *    fdatasync'd. Use it for the one thing that must survive a crash before it is acted on: an
 *    order intent, logged before the order leaves the process. If the disk fails it returns an
 *    error and the caller must NOT proceed (fail closed).
 *  - appendAsync(): enqueues and returns at once; the writer thread flushes in batches. Use it
 *    for audit detail. If the bounded queue is full the record is dropped and counted (a stalled
 *    disk must never stall order processing).
 *
 * File layout (little-endian): header "FWAL" u16 version u16 reserved, then records
 *   u32 payloadLen | payload | u32 crc32(payload)
 * A torn tail from a crash is detected on read and every complete record before it is returned.
 * The file is created 0600. Payloads are opaque bytes; see oms/journal_codec.hpp for the OMS's.
 */
inline constexpr std::uint16_t kWalVersion = 1;
inline constexpr std::size_t kMaxWalRecordBytes = 1U << 20;

struct WalConfig {
  std::string path;
  std::size_t asyncQueueLimit{100'000};
  /** Longest the writer waits to batch async records before flushing them. */
  std::chrono::milliseconds flushInterval{50};
};

struct WalStats {
  std::atomic<std::uint64_t> records{0};
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<std::uint64_t> syncs{0};
  std::atomic<std::uint64_t> droppedAsync{0};
  std::atomic<std::uint64_t> writeErrors{0};
  LatencyHistogram syncNs;  // fdatasync latency: the disk's contribution to order latency
};

class Wal {
 public:
  /**
   * Opens (creating if needed) and starts the writer thread. Appends to an existing log, after
   * checking it: a torn tail left by a crash is cut off first (otherwise every record appended
   * later would sit behind garbage and be unreachable on the next read), and a log that is corrupt
   * anywhere else is REFUSED, because a log that may be missing intents cannot protect a restart.
   */
  static Result<std::unique_ptr<Wal>> open(const WalConfig& config);
  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  Result<bool> appendDurable(const std::string& payload);
  /** False if the record was dropped (queue full or log failed). */
  bool appendAsync(const std::string& payload);
  /** Flushes and syncs everything queued so far. */
  Result<bool> flush();

  const WalStats& stats() const { return stats_; }
  /** True once a write has failed: the log can no longer be trusted to be complete. */
  bool failed() const { return failed_.load(); }

 private:
  struct Item {
    std::string payload;
    std::shared_ptr<std::promise<Result<bool>>> done;  // set for durable appends and flushes
    bool syncOnly{false};
  };

  Wal(WalConfig config, int fd);
  void run();
  bool writeAll(const std::string& bytes);
  Result<bool> syncFile();

  WalConfig config_;
  int fd_;
  WalStats stats_;
  std::atomic<bool> failed_{false};

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Item> queue_;
  bool stopping_{false};
  std::thread writer_;
};

enum class WalStatus : std::uint8_t {
  kOk,
  kTruncated,    // ends mid-record (crash during a write): earlier records are intact
  kBadChecksum,  // a record is corrupt: it and everything after it are NOT returned
  kBadHeader,
  kBadVersion,
  kBadRecord,
  kUnreadable,
};

struct WalReadResult {
  std::vector<std::string> records;
  WalStatus status{WalStatus::kOk};
  /** Byte offset just past the last intact record (8 for a header-only log). */
  std::uint64_t validBytes{0};
};

/** Reads a WAL file. A missing file is kUnreadable; an empty (header-only) log is kOk. */
WalReadResult readWal(const std::string& path);

}  // namespace futu_trader::infra
