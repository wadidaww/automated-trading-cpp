#include <gtest/gtest.h>

#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "futu_trader/opend/framing.hpp"
#include "futu_trader/opend/proto_ids.hpp"
#include "futu_trader/opend/sha1.hpp"
#include "futu_trader/opend/types.hpp"

using namespace futu_trader;
using namespace futu_trader::opend;

namespace {

std::string hex(const std::uint8_t* data, std::size_t size) {
  std::string out;
  char buf[3];
  for (std::size_t i = 0; i < size; ++i) {
    std::snprintf(buf, sizeof(buf), "%02x", data[i]);
    out += buf;
  }
  return out;
}

std::string sha1Hex(const std::string& text) {
  const auto digest = sha1(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  return hex(digest.data(), digest.size());
}

std::vector<std::uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace

// --- SHA-1 (vectors from FIPS 180 and Python hashlib; lengths chosen around padding edges) ---

TEST(Sha1, KnownVectors) {
  EXPECT_EQ(sha1Hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(sha1Hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(sha1Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(Sha1, PaddingBoundaries) {
  EXPECT_EQ(sha1Hex(std::string(55, 'a')), "c1c8bbdc22796e28c0e15163d20899b65621d65a");
  EXPECT_EQ(sha1Hex(std::string(56, 'a')), "c2db330f6083854c99d4b5bfb6e8f29f201be699");
  EXPECT_EQ(sha1Hex(std::string(63, 'a')), "03f09f5b158a7a8cdad920bddc29b81c18a551f5");
  EXPECT_EQ(sha1Hex(std::string(64, 'a')), "0098ba824b5c16427bd7a1122a5a442a25ec644d");
  EXPECT_EQ(sha1Hex(std::string(65, 'a')), "11655326c708d70319be2610e8a57d9a5b959d3b");
  EXPECT_EQ(sha1Hex(std::string(119, 'a')), "ee971065aaa017e0632a8ca6c77bb3bf8b1dfc56");
  EXPECT_EQ(sha1Hex(std::string(120, 'a')), "f34c1488385346a55709ba056ddd08280dd4c6d6");
  EXPECT_EQ(sha1Hex(std::string(128, 'a')), "ad5b3fdbcb526778c2839d2f151ea753995e26a0");
  EXPECT_EQ(sha1Hex(std::string(1000, 'a')), "291e9a6c66994949b57ba5e650361e98fc36b1ba");
}

// --- Frame layout (reference bytes produced by the official SDK's struct format) ---

TEST(Framing, EncodeMatchesSdkLayoutByteForByte) {
  const std::vector<std::uint8_t> body{0x0a, 0x02, 'h', 'i'};
  const auto frame = encodeFrame(protoId::kKeepAlive, 7, body.data(), body.size());
  ASSERT_EQ(frame.size(), kHeaderSize + body.size());
  EXPECT_EQ(hex(frame.data(), frame.size()),
            "4654ec030000000007000000040000007eeb48830a7d3a42182edb15679d451b7ca4289700000000"
            "000000000a026869");
}

TEST(Framing, ProtoIdsMatchPinnedSdkTable) {
  // futu-api 10.11.7108 futu/common/constant.py ProtoId. Note the scaffold's old values
  // (place=2201, cancel=2202, getKl=3100, snapshot=3007) were wrong.
  EXPECT_EQ(protoId::kInitConnect, 1001U);
  EXPECT_EQ(protoId::kKeepAlive, 1004U);
  EXPECT_EQ(protoId::kTrdGetAccList, 2001U);
  EXPECT_EQ(protoId::kTrdUnlockTrade, 2005U);
  EXPECT_EQ(protoId::kTrdSubAccPush, 2008U);
  EXPECT_EQ(protoId::kTrdGetFunds, 2101U);
  EXPECT_EQ(protoId::kTrdGetPositionList, 2102U);
  EXPECT_EQ(protoId::kTrdGetOrderList, 2201U);
  EXPECT_EQ(protoId::kTrdPlaceOrder, 2202U);
  EXPECT_EQ(protoId::kTrdModifyOrder, 2205U);
  EXPECT_EQ(protoId::kTrdUpdateOrder, 2208U);
  EXPECT_EQ(protoId::kTrdGetOrderFillList, 2211U);
  EXPECT_EQ(protoId::kTrdUpdateOrderFill, 2218U);
  EXPECT_EQ(protoId::kQotSub, 3001U);
  EXPECT_EQ(protoId::kQotGetBasicQot, 3004U);
  EXPECT_EQ(protoId::kQotUpdateBasicQot, 3005U);
  EXPECT_EQ(protoId::kQotGetKL, 3006U);
  EXPECT_EQ(protoId::kQotUpdateKL, 3007U);
  EXPECT_EQ(protoId::kQotUpdateTicker, 3011U);
  EXPECT_EQ(protoId::kQotUpdateOrderBook, 3013U);
  EXPECT_EQ(protoId::kQotRequestHistoryKL, 3103U);
}

TEST(FrameDecoder, RoundTripsOneFrame) {
  const auto body = bytes("hello");
  const auto wire = encodeFrame(3001, 42, body.data(), body.size());
  FrameDecoder dec;
  dec.feed(wire.data(), wire.size());
  const auto frame = dec.next();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->protoId, 3001U);
  EXPECT_EQ(frame->serial, 42U);
  EXPECT_EQ(frame->body, body);
  EXPECT_FALSE(dec.next().has_value());
  EXPECT_EQ(dec.buffered(), 0U);
}

TEST(FrameDecoder, EmptyBodyIsValid) {
  const auto wire = encodeFrame(1004, 1, nullptr, 0);
  FrameDecoder dec;
  dec.feed(wire.data(), wire.size());
  const auto frame = dec.next();
  ASSERT_TRUE(frame.has_value());
  EXPECT_TRUE(frame->body.empty());
}

TEST(FrameDecoder, ByteAtATimeDelivery) {
  const auto body = bytes(std::string(300, 'x'));
  const auto wire = encodeFrame(2202, 9, body.data(), body.size());
  FrameDecoder dec;
  for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
    dec.feed(&wire[i], 1);
    EXPECT_FALSE(dec.next().has_value()) << "frame completed early at byte " << i;
  }
  dec.feed(&wire.back(), 1);
  const auto frame = dec.next();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->body, body);
}

TEST(FrameDecoder, CoalescedFramesAreAllReturnedInOrder) {
  std::vector<std::uint8_t> wire;
  for (std::uint32_t i = 1; i <= 3; ++i) {
    const auto body = bytes("m" + std::to_string(i));
    const auto one = encodeFrame(3005, i, body.data(), body.size());
    wire.insert(wire.end(), one.begin(), one.end());
  }
  FrameDecoder dec;
  dec.feed(wire.data(), wire.size());
  for (std::uint32_t i = 1; i <= 3; ++i) {
    const auto frame = dec.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->serial, i);
  }
  EXPECT_FALSE(dec.next().has_value());
}

TEST(FrameDecoder, BadMagicPoisonsTheDecoder) {
  auto wire = encodeFrame(1, 1, nullptr, 0);
  wire[0] = 'X';
  FrameDecoder dec;
  dec.feed(wire.data(), wire.size());
  EXPECT_FALSE(dec.next().has_value());
  EXPECT_EQ(dec.error(), DecodeError::kBadMagic);
  // Even a perfectly valid frame afterwards must not be accepted: the stream is desynchronised.
  const auto good = encodeFrame(1, 2, nullptr, 0);
  dec.feed(good.data(), good.size());
  EXPECT_FALSE(dec.next().has_value());
}

TEST(FrameDecoder, OversizeLengthRejectedBeforeBufferingTheBody) {
  auto wire = encodeFrame(1, 1, nullptr, 0);
  wire[12] = wire[13] = wire[14] = wire[15] = 0xFF;  // claims a ~4 GiB body
  FrameDecoder dec(1024);
  dec.feed(wire.data(), wire.size());
  EXPECT_FALSE(dec.next().has_value());
  EXPECT_EQ(dec.error(), DecodeError::kBodyTooLarge);
}

TEST(FrameDecoder, ChecksumMismatchDetected) {
  const auto body = bytes("payload");
  auto wire = encodeFrame(1, 1, body.data(), body.size());
  wire[kHeaderSize + 2] = static_cast<std::uint8_t>(wire[kHeaderSize + 2] ^ 1U);
  FrameDecoder dec;
  dec.feed(wire.data(), wire.size());
  EXPECT_FALSE(dec.next().has_value());
  EXPECT_EQ(dec.error(), DecodeError::kBadChecksum);
}

TEST(FrameDecoder, RandomChunkingNeverChangesTheDecodedStream) {
  // Property test: for any split of a valid byte stream, the same frames come out.
  std::mt19937 rng(2026);
  for (int round = 0; round < 200; ++round) {
    std::vector<std::uint8_t> wire;
    std::vector<std::vector<std::uint8_t>> bodies;
    const int count = 1 + static_cast<int>(rng() % 6);
    for (int i = 0; i < count; ++i) {
      std::vector<std::uint8_t> body(rng() % 200);
      for (auto& b : body) {
        b = static_cast<std::uint8_t>(rng());
      }
      const auto one = encodeFrame(3005, static_cast<std::uint32_t>(i), body.data(), body.size());
      wire.insert(wire.end(), one.begin(), one.end());
      bodies.push_back(std::move(body));
    }
    FrameDecoder dec;
    std::vector<std::vector<std::uint8_t>> got;
    for (std::size_t pos = 0; pos < wire.size();) {
      const std::size_t n = std::min<std::size_t>(1 + (rng() % 90), wire.size() - pos);
      dec.feed(&wire[pos], n);
      pos += n;
      while (auto frame = dec.next()) {
        got.push_back(std::move(frame->body));
      }
    }
    ASSERT_EQ(dec.error(), DecodeError::kNone);
    ASSERT_EQ(got, bodies) << "round " << round;
  }
}

TEST(FrameDecoder, GarbageInputNeverCrashes) {
  // Run under ASan/UBSan in CI: arbitrary bytes must only ever produce an error or no frame.
  std::mt19937 rng(7);
  for (int round = 0; round < 500; ++round) {
    std::vector<std::uint8_t> junk(rng() % 400);
    for (auto& b : junk) {
      b = static_cast<std::uint8_t>(rng());
    }
    if (round % 3 == 0 && junk.size() > 2) {
      junk[0] = 'F';
      junk[1] = 'T';  // get past the magic check sometimes
    }
    FrameDecoder dec(4096);
    dec.feed(junk.data(), junk.size());
    while (dec.next()) {
    }
  }
  SUCCEED();
}

// --- Numeric conversion at the API boundary ---

TEST(Conversions, ToMinorRoundsAndRejectsBadValues) {
  EXPECT_EQ(toMinor(350.2).value(), 350200);
  EXPECT_EQ(toMinor(0.001).value(), 1);
  EXPECT_EQ(toMinor(-1.5).value(), -1500);
  EXPECT_EQ(toMinor(0.1 + 0.2).value(), 300);  // 0.30000000000000004 must not become 300.00000..1
  EXPECT_FALSE(toMinor(std::nan("")).ok());
  EXPECT_FALSE(toMinor(std::numeric_limits<double>::infinity()).ok());
  EXPECT_FALSE(toMinor(1e300).ok());
}

TEST(Conversions, ToQuantityRejectsFractions) {
  EXPECT_EQ(toQuantity(200.0).value(), 200);
  EXPECT_EQ(toQuantity(199.9999999).value(), 200);
  EXPECT_FALSE(toQuantity(1.5).ok());
  EXPECT_FALSE(toQuantity(std::nan("")).ok());
  EXPECT_FALSE(toQuantity(1e18).ok());
}
