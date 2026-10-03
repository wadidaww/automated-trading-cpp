#include "futu_trader/opend/framing.hpp"

#include <algorithm>
#include <cstring>

namespace futu_trader::opend {

namespace {

void putU32(std::uint8_t* out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>(value);
  out[1] = static_cast<std::uint8_t>(value >> 8U);
  out[2] = static_cast<std::uint8_t>(value >> 16U);
  out[3] = static_cast<std::uint8_t>(value >> 24U);
}

std::uint32_t getU32(const std::uint8_t* in) {
  return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8U) |
         (static_cast<std::uint32_t>(in[2]) << 16U) | (static_cast<std::uint32_t>(in[3]) << 24U);
}

}  // namespace

std::vector<std::uint8_t> encodeFrame(std::uint32_t protoId, std::uint32_t serial,
                                      const std::uint8_t* body, std::size_t size) {
  std::vector<std::uint8_t> out(kHeaderSize + size, 0);
  out[0] = 'F';
  out[1] = 'T';
  putU32(&out[2], protoId);
  out[6] = kFmtProtobuf;
  out[7] = 0;  // API_PROTO_VER
  putU32(&out[8], serial);
  putU32(&out[12], static_cast<std::uint32_t>(size));
  const Sha1Digest digest = sha1(body, size);
  std::copy(digest.begin(), digest.end(), out.begin() + 16);
  // bytes 36..43 reserved, already zero
  if (size > 0) {
    std::memcpy(&out[kHeaderSize], body, size);
  }
  return out;
}

void FrameDecoder::feed(const std::uint8_t* data, std::size_t size) {
  if (error_ != DecodeError::kNone || size == 0) {
    return;
  }
  // Compact consumed bytes occasionally so the buffer does not grow without bound.
  if (pos_ > 0 && pos_ >= buf_.size() / 2) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
    pos_ = 0;
  }
  buf_.insert(buf_.end(), data, data + size);
}

std::optional<Frame> FrameDecoder::next() {
  if (error_ != DecodeError::kNone || buffered() < kHeaderSize) {
    return std::nullopt;
  }
  const std::uint8_t* head = buf_.data() + pos_;
  if (head[0] != 'F' || head[1] != 'T') {
    error_ = DecodeError::kBadMagic;
    return std::nullopt;
  }
  const std::uint32_t bodyLen = getU32(head + 12);
  // Reject before waiting for the body so a hostile length cannot make us buffer gigabytes.
  if (bodyLen > maxBody_) {
    error_ = DecodeError::kBodyTooLarge;
    return std::nullopt;
  }
  if (buffered() < kHeaderSize + bodyLen) {
    return std::nullopt;
  }
  const std::uint8_t* body = head + kHeaderSize;
  const Sha1Digest digest = sha1(body, bodyLen);
  if (!std::equal(digest.begin(), digest.end(), head + 16)) {
    error_ = DecodeError::kBadChecksum;
    return std::nullopt;
  }
  Frame frame;
  frame.protoId = getU32(head + 2);
  frame.fmt = head[6];
  frame.serial = getU32(head + 8);
  frame.body.assign(body, body + bodyLen);
  pos_ += kHeaderSize + bodyLen;
  return frame;
}

}  // namespace futu_trader::opend
