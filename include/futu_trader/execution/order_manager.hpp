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

  bool submit(Order order);
  bool cancel(const std::string& orderId);
  bool modify(const std::string& orderId, Money newPriceMinor, std::int64_t newQuantity);
  OrderState state(const std::string& orderId) const;

 private:
  FutuClient& client_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Order> orders_;
  std::unordered_map<std::string, std::string> dedupe_;
};

}  // namespace futu_trader
