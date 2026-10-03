#include "futu_trader/opend/quote_decode.hpp"

#include "Common.pb.h"
#include "Qot_UpdateBasicQot.pb.h"
#include "Qot_UpdateOrderBook.pb.h"
#include "futu_trader/opend/proto_ids.hpp"
#include "futu_trader/opend/types.hpp"

namespace futu_trader::opend {
namespace {

template <typename Rsp>
Result<Rsp> parseQuotePush(const Frame& frame) {
  Rsp rsp;
  if (!rsp.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()))) {
    return Error{ErrorCode::kProtocol, "unparseable quote push"};
  }
  if (rsp.rettype() != Common::RetType_Succeed || !rsp.has_s2c()) {
    return Error{ErrorCode::kServer, "quote push carried an error"};
  }
  return rsp;
}

}  // namespace

Result<std::optional<QuoteEvent>> QuoteAssembler::onFrame(const Frame& frame, std::int64_t recvNs) {
  using Out = std::optional<QuoteEvent>;
  if (frame.protoId == protoId::kQotUpdateBasicQot) {
    const auto rsp = parseQuotePush<Qot_UpdateBasicQot::Response>(frame);
    if (!rsp) {
      return rsp.error();
    }
    // Convert everything first so a bad entry does not leave the batch half-applied.
    std::map<std::string, Money> updates;
    for (const auto& quote : rsp.value().s2c().basicqotlist()) {
      if (quote.security().market() != kQotMarketHkSecurity) {
        ++ignoredNonHk_;
        continue;
      }
      Money last = 0;
      if (!quote.issuspended()) {
        const auto price = toMinor(quote.curprice());
        if (!price) {
          return Error{ErrorCode::kProtocol, "bad last price for " + quote.security().code()};
        }
        last = price.value();
      }
      updates[quote.security().code()] = last;
    }
    for (const auto& [code, last] : updates) {
      symbols_[code].last = last > 0 ? last : 0;
    }
    return Out{};
  }

  if (frame.protoId == protoId::kQotUpdateOrderBook) {
    const auto rsp = parseQuotePush<Qot_UpdateOrderBook::Response>(frame);
    if (!rsp) {
      return rsp.error();
    }
    const auto& s2c = rsp.value().s2c();
    if (s2c.security().market() != kQotMarketHkSecurity) {
      ++ignoredNonHk_;
      return Out{};
    }
    if (s2c.orderbookasklist_size() == 0 || s2c.orderbookbidlist_size() == 0) {
      return Out{};  // one-sided or empty book: no tradable top of book
    }
    const auto& ask = s2c.orderbookasklist(0);
    const auto& bid = s2c.orderbookbidlist(0);
    const auto askPrice = toMinor(ask.price());
    const auto bidPrice = toMinor(bid.price());
    const auto askSize = toQuantity(static_cast<double>(ask.volume()));
    const auto bidSize = toQuantity(static_cast<double>(bid.volume()));
    if (!askPrice || !bidPrice || !askSize || !bidSize) {
      return Error{ErrorCode::kProtocol, "bad number in order book for " + s2c.security().code()};
    }
    if (bidPrice.value() <= 0 || askPrice.value() <= 0 || bidPrice.value() >= askPrice.value()) {
      return Out{};  // crossed, locked or non-positive: not a usable quote
    }
    const auto known = symbols_.find(s2c.security().code());
    if (known == symbols_.end() || known->second.last <= 0) {
      return Out{};  // last price unknown or suspended
    }
    QuoteEvent out;
    out.tsNs = recvNs;
    out.symbol = s2c.security().code();
    out.bid = bidPrice.value();
    out.ask = askPrice.value();
    out.last = known->second.last;
    out.bidSize = bidSize.value();
    out.askSize = askSize.value();
    return Out{std::move(out)};
  }
  return Out{};
}

}  // namespace futu_trader::opend
