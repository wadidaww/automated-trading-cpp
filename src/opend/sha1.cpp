#include "futu_trader/opend/sha1.hpp"

#include <cstring>
#include <vector>

namespace futu_trader::opend {

namespace {

constexpr std::uint32_t rotl(std::uint32_t value, unsigned bits) {
  return (value << bits) | (value >> (32U - bits));
}

void processBlock(std::array<std::uint32_t, 5>& state, const std::uint8_t* block) {
  std::array<std::uint32_t, 80> w{};
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
           (static_cast<std::uint32_t>(block[(i * 4) + 1]) << 16U) |
           (static_cast<std::uint32_t>(block[(i * 4) + 2]) << 8U) |
           static_cast<std::uint32_t>(block[(i * 4) + 3]);
  }
  for (std::size_t i = 16; i < 80; ++i) {
    w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  for (std::size_t i = 0; i < 80; ++i) {
    std::uint32_t f = 0;
    std::uint32_t k = 0;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999U;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1U;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCU;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6U;
    }
    const std::uint32_t temp = rotl(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = temp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

}  // namespace

Sha1Digest sha1(const std::uint8_t* data, std::size_t size) {
  std::array<std::uint32_t, 5> state{0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U,
                                     0xC3D2E1F0U};
  std::size_t offset = 0;
  for (; offset + 64 <= size; offset += 64) {
    processBlock(state, data + offset);
  }

  // Final block(s): remaining bytes, 0x80, zero padding, 64-bit big-endian bit length.
  std::array<std::uint8_t, 128> tail{};
  const std::size_t remaining = size - offset;
  if (remaining > 0) {
    std::memcpy(tail.data(), data + offset, remaining);
  }
  tail[remaining] = 0x80;
  const std::size_t tailSize = remaining + 1 + 8 <= 64 ? 64 : 128;
  const std::uint64_t bitLength = static_cast<std::uint64_t>(size) * 8U;
  for (std::size_t i = 0; i < 8; ++i) {
    tail[tailSize - 1 - i] = static_cast<std::uint8_t>(bitLength >> (8U * i));
  }
  processBlock(state, tail.data());
  if (tailSize == 128) {
    processBlock(state, tail.data() + 64);
  }

  Sha1Digest out{};
  for (std::size_t i = 0; i < 5; ++i) {
    out[i * 4] = static_cast<std::uint8_t>(state[i] >> 24U);
    out[(i * 4) + 1] = static_cast<std::uint8_t>(state[i] >> 16U);
    out[(i * 4) + 2] = static_cast<std::uint8_t>(state[i] >> 8U);
    out[(i * 4) + 3] = static_cast<std::uint8_t>(state[i]);
  }
  return out;
}

}  // namespace futu_trader::opend
