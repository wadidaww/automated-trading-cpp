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

  futu_trader::Order order{"o1", "700.HK", 1, 10000, futu_trader::OrderType::kLimit,
                           futu_trader::OrderState::kPending, "idemp1"};
  assert(om.submit(order));
  assert(!om.submit(order));
  assert(om.cancel("o1"));

  futu_trader::RiskEngine risk({50000, 100000, 10000, 10, 0.8});
  assert(risk.canPlace(order, std::unordered_map<std::string, futu_trader::Money>{}, 0, 0, 0));

  futu_trader::Order large{"o2", "700.HK", 100, 10000, futu_trader::OrderType::kLimit,
                           futu_trader::OrderState::kPending, "idemp2"};
  assert(!risk.canPlace(large, std::unordered_map<std::string, futu_trader::Money>{}, 0, 0, 0));

  std::vector<int> commands;
  futu_trader::FutuClientConfig apiConfig{};
  apiConfig.requireUnlockTrade = true;
  apiConfig.tradePasswordMd5 = "md5hash";
  apiConfig.apiInvoker = [&commands](int command, const std::string&) {
    commands.push_back(command);
    return futu_trader::FutuOpenApiResult{true, "ok"};
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

  assert(std::find(commands.begin(), commands.end(), futu_trader::futuOpenApiCommand::initConnect) !=
         commands.end());
  assert(std::find(commands.begin(), commands.end(), futu_trader::futuOpenApiCommand::unlockTrade) !=
         commands.end());
  assert(std::find(commands.begin(), commands.end(), futu_trader::futuOpenApiCommand::placeOrder) !=
         commands.end());
  assert(std::find(commands.begin(), commands.end(), futu_trader::futuOpenApiCommand::modifyOrder) !=
         commands.end());
  assert(std::find(commands.begin(), commands.end(), futu_trader::futuOpenApiCommand::cancelOrder) !=
         commands.end());
  assert(std::find(commands.begin(), commands.end(), futu_trader::futuOpenApiCommand::getOrderList) !=
         commands.end());
  assert(std::find(commands.begin(), commands.end(), futu_trader::futuOpenApiCommand::subscribe) !=
         commands.end());
  assert(std::find(commands.begin(), commands.end(),
                   futu_trader::futuOpenApiCommand::getSecuritySnapshot) != commands.end());
  assert(std::find(commands.begin(), commands.end(), futu_trader::futuOpenApiCommand::getKl) !=
         commands.end());
  return 0;
}
