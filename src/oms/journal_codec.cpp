#include "futu_trader/oms/journal_codec.hpp"

#include <cstring>

namespace futu_trader::oms {
namespace {

constexpr char kSubmitTag = 'S';
constexpr char kJournalTag = 'J';
constexpr std::uint8_t kCodecVersion = 1;
constexpr std::uint32_t kMaxField = 4096;

void putU64(std::string& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
  }
}

void putStr(std::string& out, const std::string& value) {
  const auto size = static_cast<std::uint32_t>(std::min<std::size_t>(value.size(), kMaxField));
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((size >> (8 * i)) & 0xFFU));
  }
  out.append(value, 0, size);
}

class Reader {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {}
  bool u8(std::uint8_t& out) {
    if (bytes_.empty()) {
      return false;
    }
    out = static_cast<std::uint8_t>(bytes_[0]);
    bytes_.remove_prefix(1);
    return true;
  }
  bool u64(std::uint64_t& out) {
    if (bytes_.size() < 8) {
      return false;
    }
    out = 0;
    for (int i = 0; i < 8; ++i) {
      out |=
          static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes_[static_cast<std::size_t>(i)]))
          << (8 * i);
    }
    bytes_.remove_prefix(8);
    return true;
  }
  bool str(std::string& out) {
    if (bytes_.size() < 4) {
      return false;
    }
    std::uint32_t size = 0;
    for (int i = 0; i < 4; ++i) {
      size |=
          static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes_[static_cast<std::size_t>(i)]))
          << (8 * i);
    }
    bytes_.remove_prefix(4);
    if (size > kMaxField || bytes_.size() < size) {
      return false;
    }
    out.assign(bytes_.substr(0, size));
    bytes_.remove_prefix(size);
    return true;
  }
  bool done() const { return bytes_.empty(); }

 private:
  std::string_view bytes_;
};

}  // namespace

std::string encodeSubmit(const DurableSubmit& submit) {
  std::string out;
  out.push_back(kSubmitTag);
  out.push_back(static_cast<char>(kCodecVersion));
  putU64(out, static_cast<std::uint64_t>(submit.tsNs));
  putStr(out, submit.clOrdId);
  putStr(out, submit.intentKey);
  putStr(out, submit.symbol);
  out.push_back(static_cast<char>(submit.side == Side::kBuy ? 0 : 1));
  putU64(out, static_cast<std::uint64_t>(submit.qty));
  putU64(out, static_cast<std::uint64_t>(submit.priceMills));
  return out;
}

std::optional<DurableSubmit> decodeSubmit(std::string_view bytes) {
  Reader in(bytes);
  std::uint8_t tag = 0;
  std::uint8_t version = 0;
  if (!in.u8(tag) || tag != static_cast<std::uint8_t>(kSubmitTag) || !in.u8(version) ||
      version != kCodecVersion) {
    return std::nullopt;
  }
  DurableSubmit out;
  std::uint64_t ts = 0;
  std::uint8_t side = 0;
  std::uint64_t qty = 0;
  std::uint64_t price = 0;
  if (!in.u64(ts) || !in.str(out.clOrdId) || !in.str(out.intentKey) || !in.str(out.symbol) ||
      !in.u8(side) || side > 1 || !in.u64(qty) || !in.u64(price) || !in.done()) {
    return std::nullopt;
  }
  out.tsNs = static_cast<std::int64_t>(ts);
  out.side = side == 0 ? Side::kBuy : Side::kSell;
  out.qty = static_cast<std::int64_t>(qty);
  out.priceMills = static_cast<Money>(price);
  return out;
}

std::string encodeJournal(const JournalEntry& entry) {
  std::string out;
  out.push_back(kJournalTag);
  out.push_back(static_cast<char>(kCodecVersion));
  putU64(out, static_cast<std::uint64_t>(entry.tsNs));
  out.push_back(static_cast<char>(entry.kind));
  putStr(out, entry.clOrdId);
  putStr(out, entry.detail);
  return out;
}

std::optional<JournalEntry> decodeJournal(std::string_view bytes) {
  Reader in(bytes);
  std::uint8_t tag = 0;
  std::uint8_t version = 0;
  if (!in.u8(tag) || tag != static_cast<std::uint8_t>(kJournalTag) || !in.u8(version) ||
      version != kCodecVersion) {
    return std::nullopt;
  }
  JournalEntry out;
  std::uint64_t ts = 0;
  std::uint8_t kind = 0;
  if (!in.u64(ts) || !in.u8(kind) || kind > static_cast<std::uint8_t>(JournalKind::kBootstrap) ||
      !in.str(out.clOrdId) || !in.str(out.detail) || !in.done()) {
    return std::nullopt;
  }
  out.tsNs = static_cast<std::int64_t>(ts);
  out.kind = static_cast<JournalKind>(kind);
  return out;
}

}  // namespace futu_trader::oms
