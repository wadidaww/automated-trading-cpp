#include "futu_trader/api/futu_client.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace futu_trader;

namespace {
Order sampleOrder() {
  Order o;
  o.orderId = "o1";
  o.symbol = "700.HK";
  o.quantity = 1;
  o.limitPriceMinor = 10000;
  return o;
}
}  // namespace

TEST(FutuClient, InvokerReceivesEachCommandAndSide) {
  std::vector<int> commands;
  std::string lastPlacePayload;
  FutuClientConfig cfg{};
  cfg.requireUnlockTrade = true;
  cfg.tradePasswordMd5 = "md5hash";
  cfg.apiInvoker = [&](int command, const std::string& payload) {
    commands.push_back(command);
    if (command == futuOpenApiCommand::placeOrder) {
      lastPlacePayload = payload;
    }
    return FutuOpenApiResult{.success = true, .payload = "ok"};
  };
  FutuClient client(cfg);
  ASSERT_TRUE(client.connect());
  ASSERT_TRUE(client.unlockTrade());
  ASSERT_TRUE(client.subscribe("700.HK"));
  ASSERT_TRUE(client.getBasicQot("700.HK").has_value());
  Order sell = sampleOrder();
  sell.side = Side::kSell;
  ASSERT_EQ(client.placeOrder(sell), "o1");
  EXPECT_NE(lastPlacePayload.find("side=sell"), std::string::npos);
  EXPECT_TRUE(client.modifyOrder("o1", 10020, 2));
  EXPECT_TRUE(client.cancelOrder("o1"));

  for (int expected : {futuOpenApiCommand::initConnect, futuOpenApiCommand::unlockTrade,
                       futuOpenApiCommand::placeOrder, futuOpenApiCommand::modifyOrder,
                       futuOpenApiCommand::cancelOrder, futuOpenApiCommand::subscribe}) {
    EXPECT_NE(std::find(commands.begin(), commands.end(), expected), commands.end()) << expected;
  }
}

TEST(FutuClient, FailedConnectDoesNotUnlockTrading) {
  // Regression: connect() used to mark trading unlocked even when the connection failed.
  FutuClientConfig cfg{};
  cfg.requireUnlockTrade = false;
  cfg.apiInvoker = [](int, const std::string&) { return FutuOpenApiResult{.success = false}; };
  FutuClient client(cfg);
  EXPECT_FALSE(client.connect());
  EXPECT_EQ(client.placeOrder(sampleOrder()), "");
}

TEST(FutuClient, PlaceOrderFailsWhenDisconnectedOrLocked) {
  FutuClientConfig cfg{};
  cfg.requireUnlockTrade = true;
  FutuClient client(cfg);
  EXPECT_EQ(client.placeOrder(sampleOrder()), "");  // not connected
  ASSERT_TRUE(client.connect());
  EXPECT_EQ(client.placeOrder(sampleOrder()), "");  // connected but trade still locked
}
