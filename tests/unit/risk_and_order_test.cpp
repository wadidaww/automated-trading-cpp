#include <cassert>
#include <unordered_map>

#include "futu_trader/api/futu_client.hpp"
#include "futu_trader/execution/order_manager.hpp"
#include "futu_trader/risk/risk_engine.hpp"

int main() {
  futu_trader::FutuClient client({});
  client.Connect();
  futu_trader::OrderManager om(client);

  futu_trader::Order order{"o1", "700.HK", 1, 10000, futu_trader::OrderType::kLimit,
                           futu_trader::OrderState::kPending, "idemp1"};
  assert(om.Submit(order));
  assert(!om.Submit(order));
  assert(om.Cancel("o1"));

  futu_trader::RiskEngine risk({50000, 100000, 10000, 10, 0.8});
  assert(risk.CanPlace(order, std::unordered_map<std::string, futu_trader::Money>{}, 0, 0, 0));

  futu_trader::Order large{"o2", "700.HK", 100, 10000, futu_trader::OrderType::kLimit,
                           futu_trader::OrderState::kPending, "idemp2"};
  assert(!risk.CanPlace(large, std::unordered_map<std::string, futu_trader::Money>{}, 0, 0, 0));
  return 0;
}
