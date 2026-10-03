#include "futu_trader/infra/wal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>

#include "futu_trader/data/event_log.hpp"  // crc32

namespace futu_trader::infra {

namespace {

void put32(std::string& out, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>(static_cast<std::uint8_t>(v >> (8U * i))));
  }
}

std::uint32_t get32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
         (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::string frame(const std::string& payload) {
  std::string out;
  out.reserve(payload.size() + 8);
  put32(out, static_cast<std::uint32_t>(payload.size()));
  out += payload;
  put32(out, data::crc32(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()));
  return out;
}

std::uint64_t nowNs() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

}  // namespace

Wal::Wal(WalConfig config, int fd) : config_(std::move(config)), fd_(fd) {}

Result<std::unique_ptr<Wal>> Wal::open(const WalConfig& config) {
  // O_APPEND: every write lands at the end even if something else touches the file offset.
  const int fd = ::open(config.path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
  if (fd < 0) {
    return Error{ErrorCode::kDisconnected,
                 "cannot open WAL '" + config.path + "': " + std::strerror(errno)};
  }
  std::unique_ptr<Wal> wal(new Wal(config, fd));
  const off_t size = ::lseek(fd, 0, SEEK_END);
  if (size == 0) {
    std::string header = "FWAL";
    header.push_back(static_cast<char>(kWalVersion & 0xFFU));
    header.push_back(static_cast<char>(kWalVersion >> 8U));
    header.push_back('\0');
    header.push_back('\0');
    if (!wal->writeAll(header) || !wal->syncFile().ok()) {
      return Error{ErrorCode::kDisconnected, "cannot write WAL header to '" + config.path + "'"};
    }
  } else if (size < 0) {
    return Error{ErrorCode::kDisconnected, "cannot stat WAL '" + config.path + "'"};
  }
  wal->writer_ = std::thread(&Wal::run, wal.get());
  return wal;
}

Wal::~Wal() {
  {
    std::scoped_lock lock(mu_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (writer_.joinable()) {
    writer_.join();  // the writer drains the queue before exiting
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

bool Wal::writeAll(const std::string& bytes) {
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::write(fd_, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    done += static_cast<std::size_t>(n);
  }
  return true;
}

Result<bool> Wal::syncFile() {
  const std::uint64_t start = nowNs();
  if (::fdatasync(fd_) != 0) {
    return Error{ErrorCode::kDisconnected,
                 std::string("WAL fdatasync failed: ") + std::strerror(errno)};
  }
  stats_.syncs.fetch_add(1, std::memory_order_relaxed);
  stats_.syncNs.record(nowNs() - start);
  return true;
}

Result<bool> Wal::appendDurable(const std::string& payload) {
  if (payload.size() > kMaxWalRecordBytes) {
    return Error{ErrorCode::kInvalidArg, "WAL record too large"};
  }
  if (failed_.load()) {
    return Error{ErrorCode::kDisconnected, "WAL has failed: refusing to log"};
  }
  auto done = std::make_shared<std::promise<Result<bool>>>();
  auto future = done->get_future();
  {
    std::scoped_lock lock(mu_);
    if (stopping_) {
      return Error{ErrorCode::kDisconnected, "WAL is closing"};
    }
    queue_.push_back({payload, done, false});
  }
  cv_.notify_one();
  return future.get();
}

bool Wal::appendAsync(const std::string& payload) {
  if (payload.size() > kMaxWalRecordBytes || failed_.load()) {
    stats_.droppedAsync.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  {
    std::scoped_lock lock(mu_);
    if (stopping_ || queue_.size() >= config_.asyncQueueLimit) {
      stats_.droppedAsync.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    queue_.push_back({payload, nullptr, false});
  }
  cv_.notify_one();
  return true;
}

Result<bool> Wal::flush() {
  auto done = std::make_shared<std::promise<Result<bool>>>();
  auto future = done->get_future();
  {
    std::scoped_lock lock(mu_);
    if (stopping_) {
      return Error{ErrorCode::kDisconnected, "WAL is closing"};
    }
    queue_.push_back({"", done, true});
  }
  cv_.notify_one();
  return future.get();
}

void Wal::run() {
  std::deque<Item> batch;
  while (true) {
    {
      std::unique_lock lock(mu_);
      cv_.wait_for(lock, config_.flushInterval, [&] { return stopping_ || !queue_.empty(); });
      batch.swap(queue_);
      if (batch.empty() && stopping_) {
        return;
      }
    }
    if (batch.empty()) {
      continue;
    }
    // One write per batch, then one fdatasync: durable appends in the batch share the cost.
    std::string bytes;
    std::size_t recordCount = 0;
    bool needSync = false;
    for (const Item& item : batch) {
      if (!item.syncOnly) {
        bytes += frame(item.payload);
        ++recordCount;
      }
      needSync = needSync || item.done != nullptr;
    }
    Result<bool> outcome = true;
    if (failed_.load()) {
      outcome = Error{ErrorCode::kDisconnected, "WAL has failed earlier: record not written"};
    } else if (!bytes.empty() && !writeAll(bytes)) {
      failed_.store(true);
      stats_.writeErrors.fetch_add(1, std::memory_order_relaxed);
      outcome =
          Error{ErrorCode::kDisconnected, std::string("WAL write failed: ") + std::strerror(errno)};
    } else {
      stats_.records.fetch_add(recordCount, std::memory_order_relaxed);
      stats_.bytes.fetch_add(bytes.size(), std::memory_order_relaxed);
      if (needSync) {
        outcome = syncFile();
        if (!outcome) {
          failed_.store(true);
          stats_.writeErrors.fetch_add(1, std::memory_order_relaxed);
        }
      } else if (!bytes.empty()) {
        (void)syncFile();  // async-only batch: still sync, but a failure is reported on next use
      }
    }
    for (Item& item : batch) {
      if (item.done) {
        item.done->set_value(outcome);
      }
    }
    batch.clear();
  }
}

WalReadResult readWal(const std::string& path) {
  WalReadResult result;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    result.status = WalStatus::kUnreadable;
    return result;
  }
  std::array<std::uint8_t, 8> header{};
  in.read(reinterpret_cast<char*>(header.data()), 8);
  if (in.gcount() != 8 || header[0] != 'F' || header[1] != 'W' || header[2] != 'A' ||
      header[3] != 'L') {
    result.status = WalStatus::kBadHeader;
    return result;
  }
  if (static_cast<std::uint16_t>(header[4] | (header[5] << 8U)) != kWalVersion) {
    result.status = WalStatus::kBadVersion;
    return result;
  }
  while (true) {
    std::array<std::uint8_t, 4> lenBytes{};
    in.read(reinterpret_cast<char*>(lenBytes.data()), 4);
    const auto got = static_cast<std::size_t>(in.gcount());
    if (got == 0) {
      return result;
    }
    if (got != 4) {
      result.status = WalStatus::kTruncated;
      return result;
    }
    const std::uint32_t len = get32(lenBytes.data());
    if (len > kMaxWalRecordBytes) {
      result.status = WalStatus::kBadRecord;  // never allocate what a corrupt length claims
      return result;
    }
    std::string payload(len, '\0');
    std::array<std::uint8_t, 4> crcBytes{};
    in.read(payload.data(), static_cast<std::streamsize>(len));
    if (static_cast<std::size_t>(in.gcount()) != len) {
      result.status = WalStatus::kTruncated;
      return result;
    }
    in.read(reinterpret_cast<char*>(crcBytes.data()), 4);
    if (in.gcount() != 4) {
      result.status = WalStatus::kTruncated;
      return result;
    }
    if (data::crc32(reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()) !=
        get32(crcBytes.data())) {
      result.status = WalStatus::kBadChecksum;
      return result;
    }
    result.records.push_back(std::move(payload));
  }
}

}  // namespace futu_trader::infra
