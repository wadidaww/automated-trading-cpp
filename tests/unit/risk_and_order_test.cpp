#include <cassert>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "futu_trader/api/futu_client.hpp"
#include "futu_trader/execution/order_manager.hpp"
#include "futu_trader/risk/risk_engine.hpp"

int main() {
  futu_trader::FutuClient client({});
  client.connect();
  futu_trader::OrderManager om(client);

  futu_trader::Order order{.orderId = "o1",
                           .symbol = "700.HK",
                           .quantity = 1,
                           .limitPriceMinor = 10000,
                           .type = futu_trader::OrderType::kLimit,
                           .state = futu_trader::OrderState::kPending,
                           .idempotencyKey = "idemp1"};
  assert(om.submit(order));
  assert(!om.submit(order));
  assert(om.cancel("o1"));

  futu_trader::RiskEngine risk({.maxPositionNotionalMinor = 50000,
                                .maxPortfolioNotionalMinor = 100000,
                                .maxDailyLossMinor = 10000,
                                .maxOpenOrders = 10,
                                .concentrationLimit = 0.8});
  assert(risk.canPlace(order, std::unordered_map<std::string, futu_trader::Money>{}, 0, 0, 0));

  futu_trader::Order large{.orderId = "o2",
                           .symbol = "700.HK",
                           .quantity = 100,
                           .limitPriceMinor = 10000,
                           .type = futu_trader::OrderType::kLimit,
                           .state = futu_trader::OrderState::kPending,
                           .idempotencyKey = "idemp2"};
  assert(!risk.canPlace(large, std::unordered_map<std::string, futu_trader::Money>{}, 0, 0, 0));

  std::vector<int> commands;
  futu_trader::FutuClientConfig apiConfig{};
  apiConfig.requireUnlockTrade = true;
  apiConfig.tradePasswordMd5 = "md5hash";
  apiConfig.apiInvoker = [&commands](int command, const std::string&) {
    commands.push_back(command);
    return futu_trader::FutuOpenApiResult{.success = true, .payload = "ok"};
  };

  futu_trader::FutuClient apiClient(apiConfig);
  assert(apiClient.connect());
  assert(apiClient.unlockTrade());
  assert(apiClient.subscribe("700.HK"));
  assert(apiClient.getBasicQot("700.HK").has_value());
  assert(apiClient.placeOrder(order) == "o1");
  assert(apiClient.modifyOrder("o1", 10020, 2));
  assert(apiClient.cancelOrder("o1"));
  (void)apiClient.getOrderList();
  (void)apiClient.getKl("700.HK", 3);

  const auto assertCommandPresent = [&commands](int command) {
    assert(std::find(commands.begin(), commands.end(), command) != commands.end());
  };
  assertCommandPresent(futu_trader::futuOpenApiCommand::initConnect);
  assertCommandPresent(futu_trader::futuOpenApiCommand::unlockTrade);
  assertCommandPresent(futu_trader::futuOpenApiCommand::placeOrder);
  assertCommandPresent(futu_trader::futuOpenApiCommand::modifyOrder);
  assertCommandPresent(futu_trader::futuOpenApiCommand::cancelOrder);
  assertCommandPresent(futu_trader::futuOpenApiCommand::getOrderList);
  assertCommandPresent(futu_trader::futuOpenApiCommand::subscribe);
  assertCommandPresent(futu_trader::futuOpenApiCommand::getSecuritySnapshot);
  assertCommandPresent(futu_trader::futuOpenApiCommand::getKl);
  return 0;
}
