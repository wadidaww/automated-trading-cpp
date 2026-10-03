#include "futu_trader/opend/trade_decode.hpp"

#include "Common.pb.h"
#include "Trd_UpdateOrder.pb.h"
#include "Trd_UpdateOrderFill.pb.h"
#include "futu_trader/opend/proto_ids.hpp"

namespace futu_trader::opend {

namespace {

Result<WireHeader> convertHeader(const Trd_Common::TrdHeader& header) {
  // Validate before casting: an out-of-range value in an enum class is undefined territory, and
  // an unexpected market/env means we do not understand what this event is about.
  if (header.trdenv() != Trd_Common::TrdEnv_Real &&
      header.trdenv() != Trd_Common::TrdEnv_Simulate) {
    return Error{ErrorCode::kProtocol, "push with unknown trading environment"};
  }
  const auto market = header.trdmarket();
  if (market != Trd_Common::TrdMarket_HK && market != Trd_Common::TrdMarket_US &&
      market != Trd_Common::TrdMarket_CN) {
    return Error{ErrorCode::kProtocol, "push for unsupported market " + std::to_string(market)};
  }
  WireHeader out;
  out.env = header.trdenv() == Trd_Common::TrdEnv_Real ? TrdEnv::kReal : TrdEnv::kSimulate;
  out.accId = header.accid();
  out.market = static_cast<TrdMarket>(market);
  return out;
}

template <typename Rsp>
Result<Rsp> parsePush(const Frame& frame, std::uint32_t expectedId) {
  if (frame.protoId != expectedId) {
    return Error{ErrorCode::kProtocol, "unexpected push proto id"};
  }
  Rsp rsp;
  if (!rsp.ParseFromArray(frame.body.data(), static_cast<int>(frame.body.size()))) {
    return Error{ErrorCode::kProtocol, "unparseable push"};
  }
  if (rsp.rettype() != Common::RetType_Succeed || !rsp.has_s2c()) {
    return Error{ErrorCode::kServer, "push carried an error"};
  }
  return rsp;
}

}  // namespace

Result<BrokerOrder> convertOrder(const Trd_Common::Order& order) {
  const auto qty = toQuantity(order.qty());
  const auto fillQty = toQuantity(order.fillqty());
  const auto price = toMinor(order.price());
  const auto avg = toMinor(order.fillavgprice());
  if (!qty || !fillQty || !price || !avg) {
    return Error{ErrorCode::kProtocol, "bad number in order " + std::to_string(order.orderid())};
  }
  BrokerOrder out;
  out.orderId = order.orderid();
  out.orderIdEx = order.orderidex();
  out.code = order.code();
  out.side = order.trdside() == Trd_Common::TrdSide_Buy ? Side::kBuy : Side::kSell;
  out.qty = qty.value();
  out.priceMills = price.value();
  out.fillQty = fillQty.value();
  out.fillAvgPriceMills = avg.value();
  out.status = order.orderstatus();
  out.remark = order.remark();
  out.updateTimestamp = order.updatetimestamp();
  return out;
}

Result<BrokerFill> convertFill(const Trd_Common::OrderFill& item) {
  const auto qty = toQuantity(item.qty());
  const auto price = toMinor(item.price());
  if (!qty || !price) {
    return Error{ErrorCode::kProtocol, "bad number in fill " + item.fillidex()};
  }
  BrokerFill out;
  out.fillId = item.fillidex();
  out.orderId = item.orderid();
  out.code = item.code();
  out.side = item.trdside() == Trd_Common::TrdSide_Buy ? Side::kBuy : Side::kSell;
  out.qty = qty.value();
  out.priceMills = price.value();
  out.status = item.status();
  return out;
}

Result<OrderUpdate> decodeOrderUpdate(const Frame& frame) {
  const auto rsp = parsePush<Trd_UpdateOrder::Response>(frame, protoId::kTrdUpdateOrder);
  if (!rsp) {
    return rsp.error();
  }
  auto order = convertOrder(rsp.value().s2c().order());
  if (!order) {
    return order.error();
  }
  auto header = convertHeader(rsp.value().s2c().header());
  if (!header) {
    return header.error();
  }
  return OrderUpdate{header.value(), std::move(order.value())};
}

Result<FillUpdate> decodeFillUpdate(const Frame& frame) {
  const auto rsp = parsePush<Trd_UpdateOrderFill::Response>(frame, protoId::kTrdUpdateOrderFill);
  if (!rsp) {
    return rsp.error();
  }
  auto fill = convertFill(rsp.value().s2c().orderfill());
  if (!fill) {
    return fill.error();
  }
  auto header = convertHeader(rsp.value().s2c().header());
  if (!header) {
    return header.error();
  }
  return FillUpdate{header.value(), std::move(fill.value())};
}

}  // namespace futu_trader::opend
