#include "mock_opend.hpp"

#include <algorithm>
#include <boost/asio.hpp>
#include <condition_variable>
#include <set>

#include "Common.pb.h"
#include "InitConnect.pb.h"
#include "KeepAlive.pb.h"
#include "Qot_GetBasicQot.pb.h"
#include "Qot_RequestHistoryKL.pb.h"
#include "Qot_Sub.pb.h"
#include "Qot_UpdateBasicQot.pb.h"
#include "Trd_GetAccList.pb.h"
#include "Trd_GetFunds.pb.h"
#include "Trd_GetOrderFillList.pb.h"
#include "Trd_GetOrderList.pb.h"
#include "Trd_GetPositionList.pb.h"
#include "Trd_ModifyOrder.pb.h"
#include "Trd_PlaceOrder.pb.h"
#include "Trd_SubAccPush.pb.h"
#include "Trd_UnlockTrade.pb.h"
#include "Trd_UpdateOrder.pb.h"
#include "Trd_UpdateOrderFill.pb.h"
#include "futu_trader/opend/framing.hpp"
#include "futu_trader/opend/proto_ids.hpp"

namespace futu_trader::mock {

namespace asio = boost::asio;

namespace {
// Best-effort socket teardown: errors here are expected and uninteresting.
template <typename T>
void ignore(const T& /*value*/) {}
}  // namespace
namespace op = futu_trader::opend;

namespace {
// One accepted connection. All shutdown/close/write calls take sockMu so they never race, and
// only the owning worker closes the socket (after its read loop ends).
struct Conn {
  explicit Conn(asio::io_context& io) : socket(io) {}
  asio::ip::tcp::socket socket;
  std::mutex sockMu;
  bool closed{false};

  void shutdown() {
    std::scoped_lock lock(sockMu);
    if (!closed) {
      boost::system::error_code ec;
      ignore(socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec));
    }
  }
  void closeSocket() {
    std::scoped_lock lock(sockMu);
    closed = true;
    boost::system::error_code ec;
    ignore(socket.close(ec));
  }
  void write(const std::vector<std::uint8_t>& bytes) {
    std::scoped_lock lock(sockMu);
    if (!closed) {
      boost::system::error_code ec;
      asio::write(socket, asio::buffer(bytes), ec);
    }
  }
};
}  // namespace

struct MockOpenD::Impl {
  asio::io_context io;
  asio::ip::tcp::acceptor acceptor{io};
  std::thread acceptThread;
  std::atomic<bool> running{false};

  mutable std::mutex mu;
  Faults faults;
  std::vector<MockKline> klines;
  std::size_t pageSize{1000};
  std::vector<MockPosition> positions;
  std::map<std::string, double> quotes;
  double cash{1000000.0};
  bool rejectSub{false};
  std::vector<std::shared_ptr<Conn>> sockets;
  std::vector<std::thread> workers;
  std::set<std::string> subscribed;
  std::atomic<std::size_t> accepted{0};
  std::atomic<std::size_t> subs{0};
  std::atomic<std::size_t> keepAlives{0};

  // Trading state (guarded by mu).
  std::vector<MockOrder> orderBook;
  std::vector<MockFill> fillBook;
  std::string unlockMd5;
  std::string nextPlaceError;
  int nextPlaceRetType{-1};
  std::vector<std::uint64_t> accPush;
  std::map<std::uint64_t, std::uint32_t> lastPacketSerial;  // replay protection per connID
  std::uint64_t nextOrderId{5000};
  std::size_t placeCount{0};
  std::size_t unlockCount{0};
  bool suppressPushes{false};

  // Takes one unit of a fault counter if available.
  bool take(int Faults::*counter) {
    std::scoped_lock lock(mu);
    if (faults.*counter > 0) {
      --(faults.*counter);
      return true;
    }
    return false;
  }

  static void writeAll(Conn& conn, const std::vector<std::uint8_t>& bytes) { conn.write(bytes); }

  void send(Conn& sock, std::uint32_t protoId, std::uint32_t serial,
            const google::protobuf::Message& msg) {
    std::chrono::milliseconds delay{0};
    {
      std::scoped_lock lock(mu);
      delay = faults.responseDelay;
    }
    if (delay.count() > 0) {
      std::this_thread::sleep_for(delay);
    }
    if (take(&Faults::dropResponses)) {
      return;
    }
    const std::string body = msg.SerializeAsString();
    if (take(&Faults::wrongProtoIdResponses)) {
      protoId += 1;
    }
    auto frame = op::encodeFrame(protoId, serial,
                                 reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    if (take(&Faults::disconnectMidFrame)) {
      frame.resize(frame.size() / 2);
      writeAll(sock, frame);
      sock.shutdown();
      return;
    }
    if (take(&Faults::corruptChecksum)) {
      frame[16] = static_cast<std::uint8_t>(frame[16] ^ 0xFFU);
    }
    if (take(&Faults::oversizeLengthFrames)) {
      frame[12] = frame[13] = frame[14] = frame[15] = 0xFF;
    }
    if (take(&Faults::coalesceWithPush)) {
      Qot_UpdateBasicQot::Response push;
      push.set_rettype(Common::RetType_Succeed);
      const std::string pbody = push.SerializeAsString();
      const auto pframe =
          op::encodeFrame(op::protoId::kQotUpdateBasicQot, 0,
                          reinterpret_cast<const std::uint8_t*>(pbody.data()), pbody.size());
      frame.insert(frame.end(), pframe.begin(), pframe.end());
    }
    if (take(&Faults::splitResponses)) {
      for (const std::uint8_t byte : frame) {
        writeAll(sock, {byte});
      }
      return;
    }
    writeAll(sock, frame);
  }

  template <typename Rsp>
  static Rsp okResponse() {
    Rsp rsp;
    rsp.set_rettype(Common::RetType_Succeed);
    return rsp;
  }

  template <typename Rsp>
  void sendError(Conn& sock, std::uint32_t protoId, std::uint32_t serial, const std::string& msg,
                 int retType = Common::RetType_Failed) {
    Rsp rsp;
    rsp.set_rettype(retType);
    rsp.set_retmsg(msg);
    send(sock, protoId, serial, rsp);
  }

  void handle(Conn& sock, const op::Frame& frame, std::size_t connIndex) {
    using namespace op::protoId;
    const auto* data = frame.body.data();
    const int size = static_cast<int>(frame.body.size());
    switch (frame.protoId) {
      case kInitConnect: {
        auto rsp = okResponse<InitConnect::Response>();
        auto* s2c = rsp.mutable_s2c();
        s2c->set_serverver(1000);
        s2c->set_loginuserid(1);
        s2c->set_connid(1000 + connIndex);
        s2c->set_connaeskey("0123456789abcdef");
        s2c->set_keepaliveinterval(10);
        send(sock, frame.protoId, frame.serial, rsp);
        break;
      }
      case kKeepAlive: {
        ++keepAlives;
        {
          std::scoped_lock lock(mu);
          if (faults.ignoreKeepAlive) {
            return;
          }
        }
        KeepAlive::Request req;
        req.ParseFromArray(data, size);
        auto rsp = okResponse<KeepAlive::Response>();
        rsp.mutable_s2c()->set_time(req.c2s().time());
        send(sock, frame.protoId, frame.serial, rsp);
        break;
      }
      case kQotSub: {
        ++subs;
        Qot_Sub::Request req;
        req.ParseFromArray(data, size);
        bool reject = false;
        {
          std::scoped_lock lock(mu);
          reject = rejectSub;
          if (!reject) {
            for (const auto& sec : req.c2s().securitylist()) {
              subscribed.insert(sec.code());
            }
          }
        }
        if (reject) {
          sendError<Qot_Sub::Response>(sock, frame.protoId, frame.serial, "quota exceeded");
        } else {
          send(sock, frame.protoId, frame.serial, okResponse<Qot_Sub::Response>());
        }
        break;
      }
      case kQotGetBasicQot: {
        Qot_GetBasicQot::Request req;
        req.ParseFromArray(data, size);
        auto rsp = okResponse<Qot_GetBasicQot::Response>();
        for (const auto& sec : req.c2s().securitylist()) {
          double price = 350.0;
          {
            std::scoped_lock lock(mu);
            const auto found = quotes.find(sec.code());
            if (found != quotes.end()) {
              price = found->second;
            }
          }
          auto* q = rsp.mutable_s2c()->add_basicqotlist();
          q->mutable_security()->set_market(sec.market());
          q->mutable_security()->set_code(sec.code());
          q->set_issuspended(false);
          q->set_listtime("2004-06-16");
          q->set_pricespread(0.2);
          q->set_updatetime("2026-09-29 10:00:00");
          q->set_highprice(price);
          q->set_openprice(price);
          q->set_lowprice(price);
          q->set_curprice(price);
          q->set_lastcloseprice(price);
          q->set_volume(1000);
          q->set_turnover(1.0);
          q->set_turnoverrate(0.1);
          q->set_amplitude(0.1);
          q->set_updatetimestamp(1.0e9);
        }
        send(sock, frame.protoId, frame.serial, rsp);
        break;
      }
      case kQotRequestHistoryKL: {
        Qot_RequestHistoryKL::Request req;
        req.ParseFromArray(data, size);
        std::size_t offset = 0;
        if (req.c2s().has_nextreqkey()) {
          offset = std::stoul(req.c2s().nextreqkey());
        }
        auto rsp = okResponse<Qot_RequestHistoryKL::Response>();
        auto* s2c = rsp.mutable_s2c();
        s2c->mutable_security()->CopyFrom(req.c2s().security());
        {
          std::scoped_lock lock(mu);
          const std::size_t end = std::min(klines.size(), offset + pageSize);
          for (std::size_t i = offset; i < end; ++i) {
            auto* kl = s2c->add_kllist();
            kl->set_time(klines[i].time);
            kl->set_isblank(false);
            kl->set_openprice(klines[i].open);
            kl->set_highprice(klines[i].high);
            kl->set_lowprice(klines[i].low);
            kl->set_closeprice(klines[i].close);
            kl->set_volume(klines[i].volume);
          }
          if (end < klines.size()) {
            s2c->set_nextreqkey(std::to_string(end));
          }
        }  // release mu before send(), which locks it again
        send(sock, frame.protoId, frame.serial, rsp);
        break;
      }
      case kTrdGetAccList: {
        auto rsp = okResponse<Trd_GetAccList::Response>();
        auto* sim = rsp.mutable_s2c()->add_acclist();
        sim->set_trdenv(Trd_Common::TrdEnv_Simulate);
        sim->set_accid(111);
        sim->add_trdmarketauthlist(Trd_Common::TrdMarket_HK);
        auto* real = rsp.mutable_s2c()->add_acclist();
        real->set_trdenv(Trd_Common::TrdEnv_Real);
        real->set_accid(222);
        real->add_trdmarketauthlist(Trd_Common::TrdMarket_HK);
        send(sock, frame.protoId, frame.serial, rsp);
        break;
      }
      case kTrdGetFunds: {
        Trd_GetFunds::Request req;
        req.ParseFromArray(data, size);
        auto rsp = okResponse<Trd_GetFunds::Response>();
        rsp.mutable_s2c()->mutable_header()->CopyFrom(req.c2s().header());
        auto* funds = rsp.mutable_s2c()->mutable_funds();
        {
          std::scoped_lock lock(mu);
          funds->set_power(cash);
          funds->set_totalassets(cash);
          funds->set_cash(cash);
          funds->set_marketval(0);
          funds->set_frozencash(0);
          funds->set_debtcash(0);
          funds->set_avlwithdrawalcash(cash);
        }
        send(sock, frame.protoId, frame.serial, rsp);
        break;
      }
      case kTrdGetPositionList: {
        Trd_GetPositionList::Request req;
        req.ParseFromArray(data, size);
        auto rsp = okResponse<Trd_GetPositionList::Response>();
        rsp.mutable_s2c()->mutable_header()->CopyFrom(req.c2s().header());
        {
          std::scoped_lock lock(mu);
          std::uint64_t id = 1;
          for (const auto& p : positions) {
            auto* pos = rsp.mutable_s2c()->add_positionlist();
            pos->set_positionid(id++);
            pos->set_positionside(p.side);
            pos->set_code(p.code);
            pos->set_name(p.code);
            pos->set_qty(p.qty);
            pos->set_cansellqty(p.canSell);
            pos->set_price(p.price);
            pos->set_costprice(p.cost);
            pos->set_val(p.qty * p.price);
            pos->set_plval(0);
          }
        }  // release mu before send()
        send(sock, frame.protoId, frame.serial, rsp);
        break;
      }
      case kTrdUnlockTrade: {
        Trd_UnlockTrade::Request req;
        req.ParseFromArray(data, size);
        bool ok = true;
        {
          std::scoped_lock lock(mu);
          ++unlockCount;
          ok = unlockMd5.empty() || req.c2s().pwdmd5() == unlockMd5;
        }
        if (ok) {
          send(sock, frame.protoId, frame.serial, okResponse<Trd_UnlockTrade::Response>());
        } else {
          sendError<Trd_UnlockTrade::Response>(sock, frame.protoId, frame.serial, "wrong password");
        }
        break;
      }
      case kTrdSubAccPush: {
        Trd_SubAccPush::Request req;
        req.ParseFromArray(data, size);
        {
          std::scoped_lock lock(mu);
          accPush.assign(req.c2s().accidlist().begin(), req.c2s().accidlist().end());
        }
        send(sock, frame.protoId, frame.serial, okResponse<Trd_SubAccPush::Response>());
        break;
      }
      case kTrdPlaceOrder: {
        Trd_PlaceOrder::Request req;
        req.ParseFromArray(data, size);
        std::string why;
        if (!checkPacket(req.c2s().packetid(), why)) {
          sendError<Trd_PlaceOrder::Response>(sock, frame.protoId, frame.serial, why);
          break;
        }
        std::string forcedError;
        int forcedRetType = Common::RetType_Failed;
        MockOrder created;
        {
          std::scoped_lock lock(mu);
          ++placeCount;
          forcedError.swap(nextPlaceError);
          forcedRetType = nextPlaceRetType;
          nextPlaceRetType = Common::RetType_Failed;
          if (forcedError.empty()) {
            created.orderId = nextOrderId++;
            created.remark = req.c2s().remark();
            created.code = req.c2s().code();
            created.trdSide = req.c2s().trdside();
            created.qty = req.c2s().qty();
            created.price = req.c2s().price();
            created.status = 5;
            created.trdEnv = req.c2s().header().trdenv();
            created.accId = req.c2s().header().accid();
            orderBook.push_back(created);
          }
        }
        if (!forcedError.empty()) {
          sendError<Trd_PlaceOrder::Response>(sock, frame.protoId, frame.serial, forcedError,
                                              forcedRetType);
          break;
        }
        auto rsp = okResponse<Trd_PlaceOrder::Response>();
        rsp.mutable_s2c()->mutable_header()->CopyFrom(req.c2s().header());
        rsp.mutable_s2c()->set_orderid(created.orderId);
        rsp.mutable_s2c()->set_orderidex("EX" + std::to_string(created.orderId));
        send(sock, frame.protoId, frame.serial,
             rsp);  // may be dropped by a fault: order still exists
        pushOrder(created);
        break;
      }
      case kTrdModifyOrder: {
        Trd_ModifyOrder::Request req;
        req.ParseFromArray(data, size);
        std::string why;
        if (!checkPacket(req.c2s().packetid(), why)) {
          sendError<Trd_ModifyOrder::Response>(sock, frame.protoId, frame.serial, why);
          break;
        }
        bool found = false;
        MockOrder updated;
        {
          std::scoped_lock lock(mu);
          for (auto& o : orderBook) {
            if (o.orderId == req.c2s().orderid()) {
              found = true;
              if (req.c2s().modifyorderop() == Trd_Common::ModifyOrderOp_Cancel) {
                o.status = 15;  // Cancelled_All
              } else {
                o.qty = req.c2s().qty();
                o.price = req.c2s().price();
              }
              updated = o;
            }
          }
        }
        if (!found) {
          sendError<Trd_ModifyOrder::Response>(sock, frame.protoId, frame.serial, "no such order");
          break;
        }
        auto rsp = okResponse<Trd_ModifyOrder::Response>();
        rsp.mutable_s2c()->mutable_header()->CopyFrom(req.c2s().header());
        rsp.mutable_s2c()->set_orderid(updated.orderId);
        send(sock, frame.protoId, frame.serial, rsp);
        pushOrder(updated);
        break;
      }
      case kTrdGetOrderList: {
        Trd_GetOrderList::Request req;
        req.ParseFromArray(data, size);
        auto rsp = okResponse<Trd_GetOrderList::Response>();
        rsp.mutable_s2c()->mutable_header()->CopyFrom(req.c2s().header());
        {
          std::scoped_lock lock(mu);
          for (const auto& o : orderBook) {
            if (o.accId == req.c2s().header().accid() && o.trdEnv == req.c2s().header().trdenv()) {
              fillOrder(rsp.mutable_s2c()->add_orderlist(), o);
            }
          }
        }
        send(sock, frame.protoId, frame.serial, rsp);
        break;
      }
      case kTrdGetOrderFillList: {
        Trd_GetOrderFillList::Request req;
        req.ParseFromArray(data, size);
        auto rsp = okResponse<Trd_GetOrderFillList::Response>();
        rsp.mutable_s2c()->mutable_header()->CopyFrom(req.c2s().header());
        {
          std::scoped_lock lock(mu);
          for (const auto& f : fillBook) {
            fillFill(rsp.mutable_s2c()->add_orderfilllist(), f);
          }
        }
        send(sock, frame.protoId, frame.serial, rsp);
        break;
      }
      default:
        break;  // unknown commands are ignored: the client should time out, not crash
    }
  }

  static void fillHeader(Trd_Common::TrdHeader* out, const MockOrder& o) {
    out->set_trdenv(o.trdEnv);
    out->set_accid(o.accId);
    out->set_trdmarket(Trd_Common::TrdMarket_HK);
  }

  static void fillOrder(Trd_Common::Order* out, const MockOrder& o) {
    out->set_trdside(o.trdSide);
    out->set_ordertype(Trd_Common::OrderType_Normal);
    out->set_orderstatus(o.status);
    out->set_orderid(o.orderId);
    out->set_orderidex("EX" + std::to_string(o.orderId));
    out->set_code(o.code);
    out->set_name(o.code);
    out->set_qty(o.qty);
    out->set_price(o.price);
    out->set_createtime("2026-09-30 10:00:00");
    out->set_updatetime("2026-09-30 10:00:01");
    out->set_fillqty(o.fillQty);
    out->set_fillavgprice(o.fillAvg);
    out->set_remark(o.remark);
    out->set_updatetimestamp(1.0e9);
  }

  static void fillFill(Trd_Common::OrderFill* out, const MockFill& f) {
    out->set_trdside(f.trdSide);
    out->set_fillid(1);
    out->set_fillidex(f.fillId);
    out->set_orderid(f.orderId);
    out->set_code(f.code);
    out->set_name(f.code);
    out->set_qty(f.qty);
    out->set_price(f.price);
    out->set_createtime("2026-09-30 10:00:02");
    out->set_status(0);
  }

  void broadcast(std::uint32_t id, const google::protobuf::Message& msg) {
    const std::string body = msg.SerializeAsString();
    const auto frame =
        op::encodeFrame(id, 0, reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    std::vector<std::shared_ptr<Conn>> targets;
    {
      std::scoped_lock lock(mu);
      if (suppressPushes) {
        return;
      }
      targets = sockets;
    }
    for (auto& conn : targets) {
      conn->write(frame);
    }
  }

  void pushOrder(const MockOrder& o) {
    Trd_UpdateOrder::Response push;
    push.set_rettype(Common::RetType_Succeed);
    fillHeader(push.mutable_s2c()->mutable_header(), o);
    fillOrder(push.mutable_s2c()->mutable_order(), o);
    broadcast(op::protoId::kTrdUpdateOrder, push);
  }

  void pushFill(const MockFill& f, const MockOrder& o) {
    Trd_UpdateOrderFill::Response push;
    push.set_rettype(Common::RetType_Succeed);
    fillHeader(push.mutable_s2c()->mutable_header(), o);
    fillFill(push.mutable_s2c()->mutable_orderfill(), f);
    broadcast(op::protoId::kTrdUpdateOrderFill, push);
  }

  // Returns false (and fills `why`) if the write's PacketID is a replay.
  bool checkPacket(const Common::PacketID& packet, std::string& why) {
    std::scoped_lock lock(mu);
    auto& last = lastPacketSerial[packet.connid()];
    if (packet.serialno() <= last) {
      why = "replayed or out-of-order packet id";
      return false;
    }
    last = packet.serialno();
    return true;
  }

  void armAccept() {
    auto sock = std::make_shared<Conn>(io);
    acceptor.async_accept(sock->socket, [this, sock](const boost::system::error_code& ec) {
      if (ec || !running.load()) {
        return;
      }
      const std::size_t index = accepted.fetch_add(1) + 1;
      {
        std::scoped_lock lock(mu);
        sockets.push_back(sock);
        workers.emplace_back([this, sock, index] { serve(sock, index); });
      }
      armAccept();
    });
  }

  void serve(const std::shared_ptr<Conn>& sock, std::size_t connIndex) {
    serveLoop(sock, connIndex);
    sock->closeSocket();  // the owning worker closes its socket, never another thread
  }

  void serveLoop(const std::shared_ptr<Conn>& sock, std::size_t connIndex) {
    op::FrameDecoder decoder;
    std::vector<std::uint8_t> chunk(4096);
    while (running.load()) {
      boost::system::error_code ec;
      const std::size_t got = sock->socket.read_some(asio::buffer(chunk), ec);
      if (ec) {
        return;
      }
      decoder.feed(chunk.data(), got);
      while (auto frame = decoder.next()) {
        handle(*sock, *frame, connIndex);
      }
      if (decoder.error() != op::DecodeError::kNone) {
        return;
      }
    }
  }
};

MockOpenD::MockOpenD() : impl_(std::make_unique<Impl>()) {}
MockOpenD::~MockOpenD() { stop(); }

std::uint16_t MockOpenD::start() {
  auto& im = *impl_;
  const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), 0);
  im.acceptor.open(endpoint.protocol());
  im.acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
  im.acceptor.bind(endpoint);
  im.acceptor.listen();
  const auto port = im.acceptor.local_endpoint().port();
  im.running = true;
  // Async accept on io.run(): closing a blocking accept() from another thread does not wake it.
  impl_->armAccept();
  im.acceptThread = std::thread([this] { impl_->io.run(); });
  return port;
}

void MockOpenD::stop() {
  auto& im = *impl_;
  if (!im.running.exchange(false)) {
    return;
  }
  boost::system::error_code ec;
  ignore(im.acceptor.close(ec));
  im.io.stop();
  dropAllConnections();
  if (im.acceptThread.joinable()) {
    im.acceptThread.join();
  }
  std::vector<std::thread> workers;
  {
    std::scoped_lock lock(im.mu);
    workers.swap(im.workers);
  }
  for (auto& worker : workers) {
    worker.join();
  }
}

void MockOpenD::setFaults(const Faults& faults) {
  std::scoped_lock lock(impl_->mu);
  impl_->faults = faults;
}
void MockOpenD::setKlines(std::vector<MockKline> klines, std::size_t pageSize) {
  std::scoped_lock lock(impl_->mu);
  impl_->klines = std::move(klines);
  impl_->pageSize = pageSize;
}
void MockOpenD::setPositions(std::vector<MockPosition> positions) {
  std::scoped_lock lock(impl_->mu);
  impl_->positions = std::move(positions);
}
void MockOpenD::setQuote(const std::string& code, double price) {
  std::scoped_lock lock(impl_->mu);
  impl_->quotes[code] = price;
}
void MockOpenD::setFundsCash(double cash) {
  std::scoped_lock lock(impl_->mu);
  impl_->cash = cash;
}
void MockOpenD::rejectNextSubscribe(bool reject) {
  std::scoped_lock lock(impl_->mu);
  impl_->rejectSub = reject;
}

void MockOpenD::setUnlockPassword(const std::string& md5) {
  std::scoped_lock lock(impl_->mu);
  impl_->unlockMd5 = md5;
}
void MockOpenD::rejectNextPlace(const std::string& message) {
  std::scoped_lock lock(impl_->mu);
  impl_->nextPlaceError = message;
}
void MockOpenD::failNextPlaceWithRetType(int retType, const std::string& message) {
  std::scoped_lock lock(impl_->mu);
  impl_->nextPlaceError = message;
  impl_->nextPlaceRetType = retType;
}
void MockOpenD::setSuppressPushes(bool suppress) {
  std::scoped_lock lock(impl_->mu);
  impl_->suppressPushes = suppress;
}
bool MockOpenD::fillOrderByRemark(const std::string& remark, double qty, double price) {
  MockOrder snapshot;
  MockFill fill;
  {
    std::scoped_lock lock(impl_->mu);
    MockOrder* target = nullptr;
    for (auto& o : impl_->orderBook) {
      if (o.remark == remark) {
        target = &o;
      }
    }
    if (target == nullptr) {
      return false;
    }
    const double total = target->fillQty + qty;
    target->fillAvg = ((target->fillAvg * target->fillQty) + (price * qty)) / total;
    target->fillQty = total;
    target->status = total >= target->qty ? 11 : 10;  // Filled_All / Filled_Part
    snapshot = *target;
    fill.fillId = "F" + std::to_string(impl_->fillBook.size() + 1);
    fill.orderId = target->orderId;
    fill.code = target->code;
    fill.trdSide = target->trdSide;
    fill.qty = qty;
    fill.price = price;
    impl_->fillBook.push_back(fill);
  }
  impl_->pushFill(fill, snapshot);
  impl_->pushOrder(snapshot);
  return true;
}
bool MockOpenD::setStatusByRemark(const std::string& remark, int status) {
  MockOrder snapshot;
  {
    std::scoped_lock lock(impl_->mu);
    bool found = false;
    for (auto& o : impl_->orderBook) {
      if (o.remark == remark) {
        o.status = status;
        snapshot = o;
        found = true;
      }
    }
    if (!found) {
      return false;
    }
  }
  impl_->pushOrder(snapshot);
  return true;
}
void MockOpenD::injectOrder(const MockOrder& order) {
  std::scoped_lock lock(impl_->mu);
  MockOrder copy = order;
  if (copy.orderId == 0) {
    copy.orderId = impl_->nextOrderId++;
  }
  impl_->orderBook.push_back(copy);
}
void MockOpenD::injectFill(const MockFill& fill) {
  std::scoped_lock lock(impl_->mu);
  impl_->fillBook.push_back(fill);
}
std::vector<MockOrder> MockOpenD::orders() const {
  std::scoped_lock lock(impl_->mu);
  return impl_->orderBook;
}
std::vector<MockFill> MockOpenD::fills() const {
  std::scoped_lock lock(impl_->mu);
  return impl_->fillBook;
}
std::size_t MockOpenD::placeRequests() const {
  std::scoped_lock lock(impl_->mu);
  return impl_->placeCount;
}
std::size_t MockOpenD::unlockRequests() const {
  std::scoped_lock lock(impl_->mu);
  return impl_->unlockCount;
}
std::vector<std::uint64_t> MockOpenD::accPushIds() const {
  std::scoped_lock lock(impl_->mu);
  return impl_->accPush;
}

void MockOpenD::dropAllConnections() {
  std::scoped_lock lock(impl_->mu);
  for (auto& sock : impl_->sockets) {
    sock->shutdown();  // wakes the worker's read; the worker closes its own socket
  }
  impl_->sockets.clear();
}

void MockOpenD::pushBasicQot(const std::string& code, double price) {
  Qot_UpdateBasicQot::Response push;
  push.set_rettype(Common::RetType_Succeed);
  auto* q = push.mutable_s2c()->add_basicqotlist();
  q->mutable_security()->set_market(1);
  q->mutable_security()->set_code(code);
  q->set_issuspended(false);
  q->set_listtime("2004-06-16");
  q->set_pricespread(0.2);
  q->set_updatetime("2026-09-29 10:00:00");
  q->set_highprice(price);
  q->set_openprice(price);
  q->set_lowprice(price);
  q->set_curprice(price);
  q->set_lastcloseprice(price);
  q->set_volume(1);
  q->set_turnover(1.0);
  q->set_turnoverrate(0.1);
  q->set_amplitude(0.1);
  const std::string body = push.SerializeAsString();
  const auto frame =
      op::encodeFrame(op::protoId::kQotUpdateBasicQot, 0,
                      reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
  std::scoped_lock lock(impl_->mu);
  for (auto& sock : impl_->sockets) {
    Impl::writeAll(*sock, frame);
  }
}

std::size_t MockOpenD::connectionCount() const { return impl_->accepted.load(); }
std::size_t MockOpenD::activeConnections() const {
  std::scoped_lock lock(impl_->mu);
  return impl_->sockets.size();
}
std::size_t MockOpenD::subscribeRequests() const { return impl_->subs.load(); }
std::size_t MockOpenD::keepAliveRequests() const { return impl_->keepAlives.load(); }
std::vector<std::string> MockOpenD::subscribedCodes() const {
  std::scoped_lock lock(impl_->mu);
  return {impl_->subscribed.begin(), impl_->subscribed.end()};
}

}  // namespace futu_trader::mock
