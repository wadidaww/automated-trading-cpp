#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "futu_trader/opend/sha1.hpp"

namespace futu_trader::opend {

/**
 * OpenD wire header (little-endian, 44 bytes):
 *   'F' 'T' | protoId u32 | fmt u8 | ver u8 | serial u32 | bodyLen u32 | sha1(body) [20] | rsv [8]
 * Layout taken from the official SDK (`MESSAGE_HEAD_FMT = "<1s1sI2B2I20s8s"`).
 */
inline constexpr std::size_t kHeaderSize = 44;
inline constexpr std::uint8_t kFmtProtobuf = 0;
inline constexpr std::size_t kDefaultMaxBody = std::size_t{16} * 1024 * 1024;

struct Frame {
  std::uint32_t protoId{0};
  std::uint32_t serial{0};
  std::uint8_t fmt{kFmtProtobuf};
  std::vector<std::uint8_t> body;
};

/** Serializes one frame (header + body), computing the body SHA-1. */
std::vector<std::uint8_t> encodeFrame(std::uint32_t protoId, std::uint32_t serial,
                                      const std::uint8_t* body, std::size_t size);

enum class DecodeError : std::uint8_t {
  kNone,
  kBadMagic,
  kBodyTooLarge,
  kBadChecksum,
};

/**
 * Incremental stream decoder. Feed arbitrary byte chunks (TCP may split or coalesce frames) and
 * pull complete frames. After any error the decoder is poisoned: the byte stream can no longer be
 * trusted, so the caller must drop the connection.
 */
class FrameDecoder {
 public:
  explicit FrameDecoder(std::size_t maxBody = kDefaultMaxBody) : maxBody_(maxBody) {}

  void feed(const std::uint8_t* data, std::size_t size);
  /** Returns the next complete frame, or nullopt if more data is needed or an error occurred. */
  std::optional<Frame> next();
  DecodeError error() const { return error_; }
  std::size_t buffered() const { return buf_.size() - pos_; }

 private:
  std::size_t maxBody_;
  std::vector<std::uint8_t> buf_;
  std::size_t pos_{0};
  DecodeError error_{DecodeError::kNone};
};

}  // namespace futu_trader::opend
