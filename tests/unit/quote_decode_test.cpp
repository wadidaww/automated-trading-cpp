#include "futu_trader/opend/quote_decode.hpp"

#include <gtest/gtest.h>

#include <cmath>

#include "Common.pb.h"
#include "Qot_UpdateBasicQot.pb.h"
#include "Qot_UpdateOrderBook.pb.h"
#include "futu_trader/opend/proto_ids.hpp"

using namespace futu_trader;
using namespace futu_trader::opend;

namespace {

Frame frameOf(std::uint32_t id, const google::protobuf::Message& message) {
  Frame frame;
  frame.protoId = id;
  const std::string bytes = message.SerializePartialAsString();
  frame.body.assign(bytes.begin(), bytes.end());
  return frame;
}

Frame basic(const std::string& code, double last, bool suspended = false, int market = 1) {
  Qot_UpdateBasicQot::Response rsp;
  rsp.set_rettype(Common::RetType_Succeed);
  auto* q = rsp.mutable_s2c()->add_basicqotlist();
  q->mutable_security()->set_market(market);
  q->mutable_security()->set_code(code);
  q->set_issuspended(suspended);
  q->set_curprice(last);
  // Required fields we do not use still have to be set for serialization.
  q->set_listtime("2004-06-16");
  q->set_pricespread(0.2);
  q->set_updatetime("2026-01-02 10:00:00");
  q->set_highprice(last);
  q->set_openprice(last);
  q->set_lowprice(last);
  q->set_lastcloseprice(last);
  q->set_volume(1);
  q->set_turnover(1);
  q->set_turnoverrate(0);
  q->set_amplitude(0);
  return frameOf(protoId::kQotUpdateBasicQot, rsp);
}

Frame book(const std::string& code, double bid, std::int64_t bidVol, double ask,
           std::int64_t askVol, int market = 1) {
  Qot_UpdateOrderBook::Response rsp;
  rsp.set_rettype(Common::RetType_Succeed);
  auto* s2c = rsp.mutable_s2c();
  s2c->mutable_security()->set_market(market);
  s2c->mutable_security()->set_code(code);
  if (bid != 0) {
    auto* b = s2c->add_orderbookbidlist();
    b->set_price(bid);
    b->set_volume(bidVol);
    b->set_oredercount(1);
  }
  if (ask != 0) {
    auto* a = s2c->add_orderbookasklist();
    a->set_price(ask);
    a->set_volume(askVol);
    a->set_oredercount(1);
  }
  return frameOf(protoId::kQotUpdateOrderBook, rsp);
}

}  // namespace

TEST(QuoteAssembler, EmitsAQuoteOnceBothLastPriceAndBookAreKnown) {
  QuoteAssembler asm_;
  auto first = asm_.onFrame(book("00700", 350.0, 4000, 350.2, 2000), 111);
  ASSERT_TRUE(first.ok());
  EXPECT_FALSE(first.value().has_value());  // no last price yet: not emitted

  ASSERT_FALSE(asm_.onFrame(basic("00700", 350.1), 112).value().has_value());
  const auto quote = asm_.onFrame(book("00700", 350.0, 4000, 350.2, 2000), 222);
  ASSERT_TRUE(quote.ok());
  ASSERT_TRUE(quote.value().has_value());
  const auto& q = *quote.value();
  EXPECT_EQ(q.symbol, "00700");
  EXPECT_EQ(q.bid, 350'000);
  EXPECT_EQ(q.ask, 350'200);
  EXPECT_EQ(q.last, 350'100);
  EXPECT_EQ(q.bidSize, 4000);
  EXPECT_EQ(q.askSize, 2000);
  EXPECT_EQ(q.tsNs, 222);  // OUR receive time, not the server's
}

TEST(QuoteAssembler, SuspendedSymbolStopsEmitting) {
  QuoteAssembler asm_;
  asm_.onFrame(basic("00700", 350.1), 1);
  ASSERT_TRUE(asm_.onFrame(book("00700", 350.0, 100, 350.2, 100), 2).value().has_value());
  asm_.onFrame(basic("00700", 350.1, /*suspended=*/true), 3);
  EXPECT_FALSE(asm_.onFrame(book("00700", 350.0, 100, 350.2, 100), 4).value().has_value());
}

TEST(QuoteAssembler, CrossedLockedOneSidedAndEmptyBooksYieldNothing) {
  QuoteAssembler asm_;
  asm_.onFrame(basic("00700", 350.1), 1);
  EXPECT_FALSE(asm_.onFrame(book("00700", 350.2, 1, 350.0, 1), 2).value().has_value());  // crossed
  EXPECT_FALSE(asm_.onFrame(book("00700", 350.0, 1, 350.0, 1), 2).value().has_value());  // locked
  EXPECT_FALSE(asm_.onFrame(book("00700", 350.0, 1, 0, 0), 2).value().has_value());      // no ask
  EXPECT_FALSE(asm_.onFrame(book("00700", 0, 0, 0, 0), 2).value().has_value());          // empty
}

TEST(QuoteAssembler, NonHkSecuritiesAreIgnoredAndCounted) {
  QuoteAssembler asm_;
  asm_.onFrame(basic("AAPL", 190.0, false, 11), 1);
  EXPECT_FALSE(asm_.onFrame(book("AAPL", 189.9, 1, 190.1, 1, 11), 2).value().has_value());
  EXPECT_EQ(asm_.ignoredNonHk(), 2U);
}

TEST(QuoteAssembler, MalformedFramesAreErrorsAndOtherFramesAreIgnored) {
  QuoteAssembler asm_;
  Frame junk;
  junk.protoId = protoId::kQotUpdateOrderBook;
  junk.body = {0xFF, 0xFF, 0xFF};
  EXPECT_FALSE(asm_.onFrame(junk, 1).ok());

  Qot_UpdateOrderBook::Response failed;
  failed.set_rettype(Common::RetType_Failed);
  EXPECT_FALSE(asm_.onFrame(frameOf(protoId::kQotUpdateOrderBook, failed), 1).ok());

  Frame other;
  other.protoId = protoId::kQotUpdateTicker;
  const auto ignored = asm_.onFrame(other, 1);
  ASSERT_TRUE(ignored.ok());
  EXPECT_FALSE(ignored.value().has_value());

  asm_.onFrame(basic("00700", 350.1), 1);
  EXPECT_FALSE(asm_.onFrame(book("00700", std::nan(""), 1, 350.2, 1), 2).ok());  // NaN price
}

TEST(QuoteAssembler, ABadLastPriceDoesNotCorruptEarlierState) {
  QuoteAssembler asm_;
  asm_.onFrame(basic("00700", 350.1), 1);
  EXPECT_FALSE(asm_.onFrame(basic("00700", std::nan("")), 2).ok());
  const auto quote = asm_.onFrame(book("00700", 350.0, 1, 350.2, 1), 3);
  ASSERT_TRUE(quote.value().has_value());
  EXPECT_EQ(quote.value()->last, 350'100);  // still the last good price
}
