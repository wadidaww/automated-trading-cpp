#include "futu_trader/opend/client.hpp"

#include <algorithm>

#include "Common.pb.h"
#include "Qot_GetBasicQot.pb.h"
#include "Qot_RequestHistoryKL.pb.h"
#include "Qot_Sub.pb.h"
#include "Trd_GetAccList.pb.h"
#include "Trd_GetFunds.pb.h"
#include "Trd_GetOrderFillList.pb.h"
#include "Trd_GetOrderList.pb.h"
#include "Trd_GetPositionList.pb.h"
#include "Trd_ModifyOrder.pb.h"
#include "Trd_PlaceOrder.pb.h"
#include "Trd_SubAccPush.pb.h"
#include "Trd_UnlockTrade.pb.h"
#include "futu_trader/opend/proto_ids.hpp"
#include "futu_trader/opend/trade_decode.hpp"

namespace futu_trader::opend {

namespace {

// Maps OpenD's retType to an error whose code says whether the request definitely failed.
// kServer / kInvalidArg mean "the gateway refused it"; anything that could mean "it may have
// been processed" (timeouts, disconnects, unknown codes) stays ambiguous so the OMS never
// treats it as a proven rejection.
Error errorFromRetType(std::int32_t retType, const std::string& message) {
  const std::string text = "OpenD error " + std::to_string(retType) + ": " + message;
  switch (retType) {
    case Common::RetType_Failed:
      return {ErrorCode::kServer, text};
    case Common::RetType_Invalid:
      return {ErrorCode::kInvalidArg, text};
    case Common::RetType_TimeOut:
      return {ErrorCode::kTimeout, text};
    case Common::RetType_DisConnect:
      return {ErrorCode::kDisconnected, text};
    default:
      return {ErrorCode::kProtocol, text};  // Unknown (-400) and anything unrecognised
  }
}

template <typename Rsp>
Result<Rsp> parseResponse(const Result<std::vector<std::uint8_t>>& raw) {
  if (!raw) {
    return raw.error();
  }
  Rsp rsp;
  if (!rsp.ParseFromArray(raw.value().data(), static_cast<int>(raw.value().size()))) {
    return Error{ErrorCode::kProtocol, "unparseable response"};
  }
  if (rsp.rettype() != Common::RetType_Succeed) {
    return errorFromRetType(rsp.rettype(), rsp.retmsg());
  }
  return rsp;
}

void fill(Qot_Common::Security* out, const SecurityRef& sec) {
  out->set_market(sec.market);
  out->set_code(sec.code);
}

void fill(Trd_Common::TrdHeader* out, const AccountHeader& header) {
  out->set_trdenv(header.env() == TrdEnv::kReal ? Trd_Common::TrdEnv_Real
                                                : Trd_Common::TrdEnv_Simulate);
  out->set_accid(header.accId());
  out->set_trdmarket(static_cast<std::int32_t>(header.market()));
}

}  // namespace

Result<SecurityRef> parseSecurity(const std::string& text) {
  const auto dot = text.find('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= text.size()) {
    return Error{ErrorCode::kInvalidArg, "expected MARKET.CODE, got '" + text + "'"};
  }
  const std::string market = text.substr(0, dot);
  SecurityRef ref;
  ref.code = text.substr(dot + 1);
  if (market == "HK") {
    ref.market = kQotMarketHkSecurity;
  } else if (market == "US") {
    ref.market = kQotMarketUsSecurity;
  } else {
    return Error{ErrorCode::kUnsupported, "unsupported market '" + market + "'"};
  }
  return ref;
}

OpenDClient::OpenDClient(ClientConfig config)
    : config_(std::move(config)), conn_(config_.connection), rng_(config_.jitterSeed) {
  conn_.setDisconnectHandler([this](const Error& error) { onDisconnect(error); });
}

OpenDClient::~OpenDClient() { close(); }

Result<SessionInfo> OpenDClient::connect() {
  auto session = conn_.connect();
  if (session) {
    if (!supervisor_.joinable() && config_.autoReconnect) {
      supervisor_ = std::thread(&OpenDClient::supervisorLoop, this);
    }
    if (onState_) {
      onState_(true);
    }
  }
  return session;
}

void OpenDClient::close() {
  {
    std::scoped_lock lock(supMu_);
    shutdown_ = true;
  }
  supCv_.notify_all();
  if (supervisor_.joinable()) {
    supervisor_.join();
  }
  conn_.close();
}

void OpenDClient::onDisconnect(const Error& /*error*/) {
  if (onState_) {
    onState_(false);
  }
  {
    std::scoped_lock lock(supMu_);
    needReconnect_ = true;
  }
  supCv_.notify_all();
}

void OpenDClient::supervisorLoop() {
  while (true) {
    {
      std::unique_lock lock(supMu_);
      supCv_.wait(lock, [&] { return needReconnect_ || shutdown_; });
      if (shutdown_) {
        return;
      }
      needReconnect_ = false;
    }
    auto delay = config_.reconnectBase;
    while (true) {
      {
        std::unique_lock lock(supMu_);
        if (supCv_.wait_for(lock, delay, [&] { return shutdown_; })) {
          return;
        }
      }
      conn_.close();  // joins the dead reader before reusing the connection object
      const auto session = conn_.connect();
      if (session) {
        // Restore subscriptions before announcing "connected" so consumers see a usable feed.
        std::vector<Subscription> subs;
        {
          std::scoped_lock lock(subMu_);
          subs = subs_;
        }
        std::vector<std::uint64_t> accIds;
        {
          std::scoped_lock lock(subMu_);
          accIds = accPush_;
        }
        bool restored = true;
        for (const auto& sub : subs) {
          if (!sendSubscribe(sub)) {
            restored = false;
            break;
          }
        }
        if (restored && !accIds.empty() && !sendAccountPush(accIds)) {
          restored = false;
        }
        if (restored) {
          ++reconnects_;
          if (onState_) {
            onState_(true);
          }
          break;
        }
        conn_.close();
      }
      // Exponential backoff with +/-25% jitter, capped.
      const auto doubled = std::min(delay * 2, config_.reconnectMax);
      std::uniform_int_distribution<long long> jitter(-doubled.count() / 4, doubled.count() / 4);
      delay = std::chrono::milliseconds(doubled.count() + jitter(rng_));
    }
  }
}

Result<bool> OpenDClient::sendSubscribe(const Subscription& sub) {
  Qot_Sub::Request req;
  auto* c2s = req.mutable_c2s();
  for (const auto& sec : sub.securities) {
    fill(c2s->add_securitylist(), sec);
  }
  for (const SubType type : sub.subTypes) {
    c2s->add_subtypelist(static_cast<std::int32_t>(type));
  }
  c2s->set_issuborunsub(true);
  c2s->set_isregorunregpush(true);
  const auto rsp =
      parseResponse<Qot_Sub::Response>(conn_.request(protoId::kQotSub, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  return true;
}

Result<bool> OpenDClient::subscribe(const std::vector<SecurityRef>& securities,
                                    const std::vector<SubType>& subTypes) {
  if (securities.empty() || subTypes.empty()) {
    return Error{ErrorCode::kInvalidArg, "nothing to subscribe"};
  }
  Subscription sub{securities, subTypes};
  auto result = sendSubscribe(sub);
  if (result) {
    std::scoped_lock lock(subMu_);
    subs_.push_back(std::move(sub));
  }
  return result;
}

Result<std::vector<BasicQuote>> OpenDClient::getBasicQuotes(
    const std::vector<SecurityRef>& securities) {
  Qot_GetBasicQot::Request req;
  for (const auto& sec : securities) {
    fill(req.mutable_c2s()->add_securitylist(), sec);
  }
  const auto rsp = parseResponse<Qot_GetBasicQot::Response>(
      conn_.request(protoId::kQotGetBasicQot, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  std::vector<BasicQuote> out;
  for (const auto& quote : rsp.value().s2c().basicqotlist()) {
    const auto price = toMinor(quote.curprice());
    const auto last = toMinor(quote.lastcloseprice());
    if (!price || !last) {
      return Error{ErrorCode::kProtocol, "bad price in quote for " + quote.security().code()};
    }
    BasicQuote basic;
    basic.security = {quote.security().market(), quote.security().code()};
    basic.curPrice = price.value();
    basic.lastClose = last.value();
    basic.volume = quote.volume();
    basic.suspended = quote.issuspended();
    basic.updateTimestamp = quote.updatetimestamp();
    out.push_back(std::move(basic));
  }
  return out;
}

Result<std::vector<Bar>> OpenDClient::requestHistoryKl(const SecurityRef& security, KlType klType,
                                                       const std::string& begin,
                                                       const std::string& end, int maxPages) {
  std::vector<Bar> bars;
  std::string nextKey;
  for (int page = 0; page < maxPages; ++page) {
    Qot_RequestHistoryKL::Request req;
    auto* c2s = req.mutable_c2s();
    c2s->set_rehabtype(Qot_Common::RehabType_Forward);
    c2s->set_kltype(static_cast<std::int32_t>(klType));
    fill(c2s->mutable_security(), security);
    c2s->set_begintime(begin);
    c2s->set_endtime(end);
    if (!nextKey.empty()) {
      c2s->set_nextreqkey(nextKey);
    }
    const auto rsp = parseResponse<Qot_RequestHistoryKL::Response>(
        conn_.request(protoId::kQotRequestHistoryKL, req.SerializeAsString()));
    if (!rsp) {
      return rsp.error();
    }
    for (const auto& kl : rsp.value().s2c().kllist()) {
      if (kl.isblank()) {
        continue;
      }
      const auto open = toMinor(kl.openprice());
      const auto high = toMinor(kl.highprice());
      const auto low = toMinor(kl.lowprice());
      const auto close = toMinor(kl.closeprice());
      if (!open || !high || !low || !close) {
        return Error{ErrorCode::kProtocol, "bad price in bar at " + kl.time()};
      }
      bars.push_back(
          Bar{kl.time(), open.value(), high.value(), low.value(), close.value(), kl.volume()});
    }
    if (!rsp.value().s2c().has_nextreqkey() || rsp.value().s2c().nextreqkey().empty()) {
      return bars;
    }
    nextKey = rsp.value().s2c().nextreqkey();
  }
  return Error{ErrorCode::kProtocol, "history paging exceeded maxPages"};
}

void OpenDClient::fillPacketId(Common::PacketID* packetId) {
  packetId->set_connid(conn_.session().connId);
  packetId->set_serialno(tradeSerial_.fetch_add(1));
}

Result<bool> OpenDClient::unlockTrade(bool unlock, const std::string& pwdMd5) {
  Trd_UnlockTrade::Request req;
  req.mutable_c2s()->set_unlock(unlock);
  if (unlock) {
    req.mutable_c2s()->set_pwdmd5(pwdMd5);
  }
  const auto rsp = parseResponse<Trd_UnlockTrade::Response>(
      conn_.request(protoId::kTrdUnlockTrade, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  return true;
}

Result<bool> OpenDClient::unlockTrade(const infra::Secret& pwdMd5) {
  if (pwdMd5.empty()) {
    return Error{ErrorCode::kInvalidArg, "trade password hash is empty"};
  }
  return unlockTrade(true, std::string(pwdMd5.reveal()));
}

Result<bool> OpenDClient::sendAccountPush(const std::vector<std::uint64_t>& accIds) {
  Trd_SubAccPush::Request req;
  for (const auto id : accIds) {
    req.mutable_c2s()->add_accidlist(id);
  }
  const auto rsp = parseResponse<Trd_SubAccPush::Response>(
      conn_.request(protoId::kTrdSubAccPush, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  return true;
}

Result<bool> OpenDClient::subscribeAccountPush(const std::vector<std::uint64_t>& accIds) {
  if (accIds.empty()) {
    return Error{ErrorCode::kInvalidArg, "no accounts to subscribe"};
  }
  auto result = sendAccountPush(accIds);
  if (result) {
    std::scoped_lock lock(subMu_);
    accPush_ = accIds;
  }
  return result;
}

Result<PlacedOrder> OpenDClient::placeOrder(const AccountHeader& header,
                                            const PlaceOrderRequest& request) {
  if (request.qty <= 0 || request.priceMills <= 0 || request.code.empty()) {
    return Error{ErrorCode::kInvalidArg, "invalid order request"};
  }
  Trd_PlaceOrder::Request req;
  auto* c2s = req.mutable_c2s();
  fillPacketId(c2s->mutable_packetid());
  fill(c2s->mutable_header(), header);
  const bool buy = request.side == Side::kBuy;
  auto trdSide = Trd_Common::TrdSide_Buy;
  if (!buy) {
    trdSide = request.sellShort ? Trd_Common::TrdSide_SellShort : Trd_Common::TrdSide_Sell;
  }
  c2s->set_trdside(trdSide);
  c2s->set_ordertype(Trd_Common::OrderType_Normal);
  c2s->set_code(request.code);
  c2s->set_qty(static_cast<double>(request.qty));
  c2s->set_price(static_cast<double>(request.priceMills) / static_cast<double>(kMoneyScale));
  c2s->set_secmarket(Trd_Common::TrdSecMarket_HK);
  c2s->set_remark(request.remark);
  c2s->set_timeinforce(Trd_Common::TimeInForce_DAY);
  const auto rsp = parseResponse<Trd_PlaceOrder::Response>(
      conn_.request(protoId::kTrdPlaceOrder, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  if (!rsp.value().has_s2c() || !rsp.value().s2c().has_orderid()) {
    return Error{ErrorCode::kProtocol, "PlaceOrder: response carried no order id"};
  }
  return PlacedOrder{rsp.value().s2c().orderid(), rsp.value().s2c().orderidex()};
}

namespace {
Result<bool> sendModify(OpenDConnection& conn, Trd_ModifyOrder::Request& req) {
  const auto rsp = parseResponse<Trd_ModifyOrder::Response>(
      conn.request(protoId::kTrdModifyOrder, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  return true;
}
}  // namespace

Result<bool> OpenDClient::cancelOrder(const AccountHeader& header, std::uint64_t orderId) {
  Trd_ModifyOrder::Request req;
  auto* c2s = req.mutable_c2s();
  fillPacketId(c2s->mutable_packetid());
  fill(c2s->mutable_header(), header);
  c2s->set_orderid(orderId);
  c2s->set_modifyorderop(Trd_Common::ModifyOrderOp_Cancel);
  return sendModify(conn_, req);
}

Result<bool> OpenDClient::modifyOrder(const AccountHeader& header, std::uint64_t orderId,
                                      std::int64_t qty, Money priceMills) {
  if (qty <= 0 || priceMills <= 0) {
    return Error{ErrorCode::kInvalidArg, "invalid modify request"};
  }
  Trd_ModifyOrder::Request req;
  auto* c2s = req.mutable_c2s();
  fillPacketId(c2s->mutable_packetid());
  fill(c2s->mutable_header(), header);
  c2s->set_orderid(orderId);
  c2s->set_modifyorderop(Trd_Common::ModifyOrderOp_Normal);
  c2s->set_qty(static_cast<double>(qty));
  c2s->set_price(static_cast<double>(priceMills) / static_cast<double>(kMoneyScale));
  return sendModify(conn_, req);
}

Result<std::vector<BrokerOrder>> OpenDClient::getOrderList(const AccountHeader& header) {
  Trd_GetOrderList::Request req;
  fill(req.mutable_c2s()->mutable_header(), header);
  req.mutable_c2s()->set_refreshcache(true);  // reconciliation must not read a stale cache
  const auto rsp = parseResponse<Trd_GetOrderList::Response>(
      conn_.request(protoId::kTrdGetOrderList, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  std::vector<BrokerOrder> out;
  for (const auto& order : rsp.value().s2c().orderlist()) {
    auto converted = convertOrder(order);
    if (!converted) {
      return converted.error();
    }
    out.push_back(std::move(converted.value()));
  }
  return out;
}

Result<std::vector<BrokerFill>> OpenDClient::getOrderFillList(const AccountHeader& header) {
  Trd_GetOrderFillList::Request req;
  fill(req.mutable_c2s()->mutable_header(), header);
  req.mutable_c2s()->set_refreshcache(true);
  const auto rsp = parseResponse<Trd_GetOrderFillList::Response>(
      conn_.request(protoId::kTrdGetOrderFillList, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  std::vector<BrokerFill> out;
  for (const auto& item : rsp.value().s2c().orderfilllist()) {
    auto converted = convertFill(item);
    if (!converted) {
      return converted.error();
    }
    out.push_back(std::move(converted.value()));
  }
  return out;
}

Result<std::vector<TrdAccount>> OpenDClient::getAccList() {
  Trd_GetAccList::Request req;
  req.mutable_c2s()->set_userid(0);
  const auto rsp = parseResponse<Trd_GetAccList::Response>(
      conn_.request(protoId::kTrdGetAccList, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  std::vector<TrdAccount> out;
  for (const auto& acc : rsp.value().s2c().acclist()) {
    TrdAccount account;
    account.env = acc.trdenv() == Trd_Common::TrdEnv_Real ? TrdEnv::kReal : TrdEnv::kSimulate;
    account.accId = acc.accid();
    account.markets.assign(acc.trdmarketauthlist().begin(), acc.trdmarketauthlist().end());
    out.push_back(std::move(account));
  }
  return out;
}

Result<FundsInfo> OpenDClient::getFunds(const AccountHeader& header) {
  Trd_GetFunds::Request req;
  fill(req.mutable_c2s()->mutable_header(), header);
  const auto rsp = parseResponse<Trd_GetFunds::Response>(
      conn_.request(protoId::kTrdGetFunds, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  if (!rsp.value().s2c().has_funds()) {
    return Error{ErrorCode::kProtocol, "GetFunds: no funds in response"};
  }
  const auto& funds = rsp.value().s2c().funds();
  const auto power = toMinor(funds.power());
  const auto total = toMinor(funds.totalassets());
  const auto cash = toMinor(funds.cash());
  const auto market = toMinor(funds.marketval());
  const auto frozen = toMinor(funds.frozencash());
  if (!power || !total || !cash || !market || !frozen) {
    return Error{ErrorCode::kProtocol, "GetFunds: bad amount"};
  }
  return FundsInfo{power.value(), total.value(), cash.value(), market.value(), frozen.value()};
}

Result<std::vector<PositionInfo>> OpenDClient::getPositions(const AccountHeader& header) {
  Trd_GetPositionList::Request req;
  fill(req.mutable_c2s()->mutable_header(), header);
  const auto rsp = parseResponse<Trd_GetPositionList::Response>(
      conn_.request(protoId::kTrdGetPositionList, req.SerializeAsString()));
  if (!rsp) {
    return rsp.error();
  }
  std::vector<PositionInfo> out;
  for (const auto& pos : rsp.value().s2c().positionlist()) {
    const auto qty = toQuantity(pos.qty());
    const auto sellable = toQuantity(pos.cansellqty());
    const auto price = toMinor(pos.price());
    const auto cost = toMinor(pos.costprice());
    if (!qty || !sellable || !price || !cost) {
      return Error{ErrorCode::kProtocol, "GetPositionList: bad value for " + pos.code()};
    }
    // Futu reports a short as a positive qty plus PositionSide_Short. Make the sign explicit, and
    // fail closed on an unknown side: guessing "long" for a short would invert our exposure.
    std::int64_t signedQty = qty.value();
    if (pos.positionside() == Trd_Common::PositionSide_Short) {
      signedQty = -signedQty;
    } else if (pos.positionside() != Trd_Common::PositionSide_Long) {
      return Error{ErrorCode::kProtocol,
                   "GetPositionList: unknown position side for " + pos.code()};
    }
    out.push_back(
        PositionInfo{pos.code(), signedQty, sellable.value(), cost.value(), price.value()});
  }
  return out;
}

}  // namespace futu_trader::opend
