#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "futu_trader/api/futu_client.hpp"
#include "futu_trader/core/types.hpp"

namespace futu_trader {

class OrderManager {
 public:
  explicit OrderManager(FutuClient& client);

  bool Submit(Order order);
  bool Cancel(const std::string& order_id);
  bool Modify(const std::string& order_id, Money new_price_minor, std::int64_t new_quantity);
  OrderState State(const std::string& order_id) const;

 private:
  FutuClient& client_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Order> orders_;
  std::unordered_map<std::string, std::string> dedupe_;
};

}  // namespace futu_trader
