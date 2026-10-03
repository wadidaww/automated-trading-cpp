#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

#include "Trd_UpdateOrder.pb.h"
#include "futu_trader/oms/order_state.hpp"
#include "futu_trader/opend/client.hpp"
#include "futu_trader/opend/proto_ids.hpp"
#include "futu_trader/opend/trade_decode.hpp"
#include "mock_opend.hpp"
#include "test_support.hpp"

using namespace futu_trader;
using testing_support::waitFor;
using namespace futu_trader::opend;
using namespace std::chrono_literals;

namespace {

ClientConfig configFor(std::uint16_t port) {
  ClientConfig cfg;
  cfg.connection.port = port;
  cfg.connection.requestTimeout = 600ms;
  cfg.reconnectBase = 20ms;
  cfg.reconnectMax = 200ms;
  return cfg;
}

PlaceOrderRequest buy(const std::string& remark, std::int64_t qty = 100, Money price = 350'200) {
  PlaceOrderRequest req;
  req.code = "00700";
  req.side = Side::kBuy;
  req.qty = qty;
  req.priceMills = price;
  req.remark = remark;
  return req;
}

const AccountHeader kSim = AccountHeader::simulate(111, TrdMarket::kHK);
// A REAL header cannot be built directly: it comes from a TradeTarget approved by the live gate.
const AccountHeader kReal = testing_support::makeRealTarget(222).header();

class OpenDTradeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    port = server.start();
    client = std::make_unique<OpenDClient>(configFor(port));
  }
  void TearDown() override {
    client.reset();
    server.stop();
  }
  mock::MockOpenD server;
  std::uint16_t port{0};
  std::unique_ptr<OpenDClient> client;
};

}  // namespace

TEST_F(OpenDTradeTest, UnlockSucceedsWithRightPasswordAndFailsWithWrong) {
  server.setUnlockPassword("abc123md5");
  ASSERT_TRUE(client->connect().ok());
  const auto bad = client->unlockTrade(true, "wrong");
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.error().code, ErrorCode::kServer);
  EXPECT_TRUE(client->unlockTrade(true, "abc123md5").ok());
  EXPECT_EQ(server.unlockRequests(), 2U);
}

TEST_F(OpenDTradeTest, PlaceOrderCarriesRemarkSideAndPriceAsDoubles) {
  ASSERT_TRUE(client->connect().ok());
  const auto placed = client->placeOrder(kSim, buy("CL-1"));
  ASSERT_TRUE(placed.ok()) << placed.error().message;
  EXPECT_EQ(placed.value().orderId, 5000U);
  const auto orders = server.orders();
  ASSERT_EQ(orders.size(), 1U);
  EXPECT_EQ(orders[0].remark, "CL-1");
  EXPECT_EQ(orders[0].code, "00700");
  EXPECT_EQ(orders[0].trdSide, 1);
  EXPECT_DOUBLE_EQ(orders[0].qty, 100.0);
  EXPECT_DOUBLE_EQ(orders[0].price, 350.2);
}

TEST_F(OpenDTradeTest, SellAndSellShortUseDistinctTrdSides) {
  ASSERT_TRUE(client->connect().ok());
  auto sell = buy("CL-S");
  sell.side = Side::kSell;
  ASSERT_TRUE(client->placeOrder(kSim, sell).ok());
  sell.remark = "CL-SS";
  sell.sellShort = true;
  ASSERT_TRUE(client->placeOrder(kSim, sell).ok());
  const auto orders = server.orders();
  ASSERT_EQ(orders.size(), 2U);
  EXPECT_EQ(orders[0].trdSide, 2);
  EXPECT_EQ(orders[1].trdSide, 3);
}

TEST_F(OpenDTradeTest, HeaderEnvironmentReachesTheBrokerUnchanged) {
  ASSERT_TRUE(client->connect().ok());
  ASSERT_TRUE(client->placeOrder(kSim, buy("A")).ok());
  ASSERT_TRUE(client->placeOrder(kReal, buy("B")).ok());
  const auto orders = server.orders();
  ASSERT_EQ(orders.size(), 2U);
  EXPECT_EQ(orders[0].trdEnv, 0);
  EXPECT_EQ(orders[0].accId, 111U);
  EXPECT_EQ(orders[1].trdEnv, 1);
  EXPECT_EQ(orders[1].accId, 222U);
}

TEST_F(OpenDTradeTest, WritePacketSerialsAlwaysIncreaseSoNothingLooksLikeAReplay) {
  // The mock rejects any write whose PacketID serial does not exceed the previous one.
  ASSERT_TRUE(client->connect().ok());
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(client->placeOrder(kSim, buy("R" + std::to_string(i))).ok()) << i;
  }
  ASSERT_TRUE(client->cancelOrder(kSim, 5000).ok());
  ASSERT_TRUE(client->modifyOrder(kSim, 5001, 200, 350'400).ok());
}

TEST_F(OpenDTradeTest, BrokerRejectionIsADefiniteServerError) {
  ASSERT_TRUE(client->connect().ok());
  server.rejectNextPlace("insufficient buying power");
  const auto rejected = client->placeOrder(kSim, buy("X"));
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, ErrorCode::kServer);
  EXPECT_NE(rejected.error().message.find("insufficient buying power"), std::string::npos);
  EXPECT_TRUE(server.orders().empty());
}

TEST_F(OpenDTradeTest, GatewayTimeoutAndDisconnectCodesAreAmbiguousNotRefusals) {
  // Review finding: only a real refusal proves "the order does not exist". OpenD's own timeout /
  // disconnect / unknown return codes could mean the order WAS processed.
  ASSERT_TRUE(client->connect().ok());
  struct Case {
    int retType;
    ErrorCode expected;
  };
  for (const Case c : {Case{-1, ErrorCode::kServer}, Case{-500, ErrorCode::kInvalidArg},
                       Case{-100, ErrorCode::kTimeout}, Case{-200, ErrorCode::kDisconnected},
                       Case{-400, ErrorCode::kProtocol}, Case{-777, ErrorCode::kProtocol}}) {
    server.failNextPlaceWithRetType(c.retType, "simulated");
    const auto result = client->placeOrder(kSim, buy("RT" + std::to_string(-c.retType)));
    ASSERT_FALSE(result.ok()) << c.retType;
    EXPECT_EQ(result.error().code, c.expected) << c.retType;
  }
}

TEST_F(OpenDTradeTest, InvalidRequestsNeverReachTheWire) {
  ASSERT_TRUE(client->connect().ok());
  EXPECT_FALSE(client->placeOrder(kSim, buy("Z", 0)).ok());
  EXPECT_FALSE(client->placeOrder(kSim, buy("Z", 100, 0)).ok());
  auto empty = buy("Z");
  empty.code.clear();
  EXPECT_FALSE(client->placeOrder(kSim, empty).ok());
  EXPECT_EQ(server.placeRequests(), 0U);
}

TEST_F(OpenDTradeTest, DroppedResponseIsAmbiguousBecauseTheOrderStillExists) {
  ASSERT_TRUE(client->connect().ok());
  mock::Faults faults;
  faults.dropResponses = 1;
  server.setFaults(faults);
  const auto lost = client->placeOrder(kSim, buy("AMBIG-1"));
  ASSERT_FALSE(lost.ok());
  EXPECT_EQ(lost.error().code, ErrorCode::kTimeout);
  // The core hazard: the client saw a failure but the broker holds the order.
  ASSERT_EQ(server.orders().size(), 1U);
  EXPECT_EQ(server.orders()[0].remark, "AMBIG-1");
}

TEST_F(OpenDTradeTest, OrderListShowsOrdersOnlyForTheRequestedAccountAndEnv) {
  ASSERT_TRUE(client->connect().ok());
  ASSERT_TRUE(client->placeOrder(kSim, buy("SIM-1")).ok());
  ASSERT_TRUE(client->placeOrder(kReal, buy("REAL-1")).ok());
  const auto sim = client->getOrderList(kSim);
  ASSERT_TRUE(sim.ok());
  ASSERT_EQ(sim.value().size(), 1U);
  EXPECT_EQ(sim.value()[0].remark, "SIM-1");
  EXPECT_EQ(sim.value()[0].qty, 100);
  EXPECT_EQ(sim.value()[0].priceMills, 350'200);
  EXPECT_EQ(*oms::fromFutuStatus(sim.value()[0].status), oms::OmsState::kWorking);
}

TEST_F(OpenDTradeTest, CancelChangesBrokerStatus) {
  ASSERT_TRUE(client->connect().ok());
  const auto placed = client->placeOrder(kSim, buy("C-1"));
  ASSERT_TRUE(placed.ok());
  ASSERT_TRUE(client->cancelOrder(kSim, placed.value().orderId).ok());
  EXPECT_EQ(server.orders()[0].status, 15);
  EXPECT_FALSE(client->cancelOrder(kSim, 999999).ok());  // unknown order is a server error
}

TEST_F(OpenDTradeTest, OrderAndFillPushesDecodeToDomainTypes) {
  std::mutex mu;
  std::vector<OrderUpdate> orderUpdates;
  std::vector<FillUpdate> fillUpdates;
  client->setPushHandler([&](const Frame& frame) {
    std::scoped_lock lock(mu);
    if (frame.protoId == protoId::kTrdUpdateOrder) {
      auto decoded = decodeOrderUpdate(frame);
      ASSERT_TRUE(decoded.ok()) << decoded.error().message;
      orderUpdates.push_back(decoded.value());
    } else if (frame.protoId == protoId::kTrdUpdateOrderFill) {
      auto decoded = decodeFillUpdate(frame);
      ASSERT_TRUE(decoded.ok()) << decoded.error().message;
      fillUpdates.push_back(decoded.value());
    }
  });
  ASSERT_TRUE(client->connect().ok());
  ASSERT_TRUE(client->subscribeAccountPush({111}).ok());
  ASSERT_TRUE(client->placeOrder(kSim, buy("P-1", 200)).ok());
  ASSERT_TRUE(server.fillOrderByRemark("P-1", 100, 350.2));

  ASSERT_TRUE(waitFor([&] {
    std::scoped_lock lock(mu);
    return orderUpdates.size() >= 2 && fillUpdates.size() >= 1;  // ack + partial-fill
  }));
  std::scoped_lock lock(mu);
  EXPECT_EQ(fillUpdates[0].fill.qty, 100);
  EXPECT_EQ(fillUpdates[0].fill.priceMills, 350'200);
  EXPECT_EQ(fillUpdates[0].fill.fillId, "F1");
  EXPECT_EQ(fillUpdates[0].header.env, TrdEnv::kSimulate);
  EXPECT_EQ(fillUpdates[0].header.accId, 111U);
  const auto& last = orderUpdates.back();
  EXPECT_EQ(last.order.remark, "P-1");
  EXPECT_EQ(last.order.fillQty, 100);
  EXPECT_EQ(*oms::fromFutuStatus(last.order.status), oms::OmsState::kPartiallyFilled);
}

TEST(TradeDecode, RejectsUnknownEnvironmentAndUnsupportedMarket) {
  const auto encode = [](int env, int market) {
    Trd_UpdateOrder::Response rsp;
    rsp.set_rettype(Common::RetType_Succeed);
    auto* header = rsp.mutable_s2c()->mutable_header();
    header->set_trdenv(env);
    header->set_accid(111);
    header->set_trdmarket(market);
    auto* order = rsp.mutable_s2c()->mutable_order();
    order->set_trdside(1);
    order->set_ordertype(1);
    order->set_orderstatus(5);
    order->set_orderid(1);
    order->set_orderidex("1");
    order->set_code("00700");
    order->set_name("x");
    order->set_qty(100);
    order->set_createtime("t");
    order->set_updatetime("t");
    const std::string body = rsp.SerializeAsString();
    Frame frame;
    frame.protoId = protoId::kTrdUpdateOrder;
    frame.body.assign(body.begin(), body.end());
    return frame;
  };
  EXPECT_TRUE(decodeOrderUpdate(encode(1, 1)).ok());
  EXPECT_FALSE(decodeOrderUpdate(encode(7, 1)).ok());   // unknown environment
  EXPECT_FALSE(decodeOrderUpdate(encode(0, 99)).ok());  // market outside the supported set
  EXPECT_FALSE(decodeOrderUpdate(encode(0, 0)).ok());
}

TEST(TradeDecode, RejectsWrongIdAndGarbage) {
  Frame wrongId;
  wrongId.protoId = protoId::kQotUpdateBasicQot;
  EXPECT_FALSE(decodeOrderUpdate(wrongId).ok());
  Frame garbage;
  garbage.protoId = protoId::kTrdUpdateOrder;
  garbage.body = {0xFF, 0xFF, 0xFF, 0x01, 0x02};
  EXPECT_FALSE(decodeOrderUpdate(garbage).ok());
  garbage.protoId = protoId::kTrdUpdateOrderFill;
  EXPECT_FALSE(decodeFillUpdate(garbage).ok());
}

TEST_F(OpenDTradeTest, AccountPushSubscriptionSurvivesReconnect) {
  ASSERT_TRUE(client->connect().ok());
  ASSERT_TRUE(client->subscribeAccountPush({111, 222}).ok());
  EXPECT_EQ(server.accPushIds(), (std::vector<std::uint64_t>{111, 222}));
  server.dropAllConnections();
  ASSERT_TRUE(waitFor([&] { return client->reconnectCount() >= 1 && client->isConnected(); }));
  EXPECT_EQ(server.accPushIds(), (std::vector<std::uint64_t>{111, 222}));
  // Serials must keep increasing across the reconnect (the mock also tracks the new connection).
  EXPECT_TRUE(client->placeOrder(kSim, buy("AFTER")).ok());
}
