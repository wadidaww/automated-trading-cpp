#include "futu_trader/execution/order_manager.hpp"

namespace futu_trader {

OrderManager::OrderManager(FutuClient& client) : client_(client) {}

bool OrderManager::submit(Order order) {
  std::scoped_lock lock(mu_);
  if (!order.idempotencyKey.empty() && dedupe_.contains(order.idempotencyKey)) {
    return false;
  }
  client_.placeOrder(order);
  order.state = OrderState::kSubmitted;
  if (!order.idempotencyKey.empty()) {
    dedupe_[order.idempotencyKey] = order.orderId;
  }
  orders_[order.orderId] = order;
  return true;
}

bool OrderManager::cancel(const std::string& orderId) {
  std::scoped_lock lock(mu_);
  if (!orders_.contains(orderId)) {
    return false;
  }
  if (!client_.cancelOrder(orderId)) {
    return false;
  }
  orders_[orderId].state = OrderState::kCancelled;
  return true;
}

bool OrderManager::modify(const std::string& orderId, Money newPriceMinor,
                          std::int64_t newQuantity) {
  std::scoped_lock lock(mu_);
  if (!orders_.contains(orderId)) {
    return false;
  }
  if (!client_.modifyOrder(orderId, newPriceMinor, newQuantity)) {
    return false;
  }
  orders_[orderId].limitPriceMinor = newPriceMinor;
  orders_[orderId].quantity = newQuantity;
  return true;
}

OrderState OrderManager::state(const std::string& orderId) const {
  std::scoped_lock lock(mu_);
  auto it = orders_.find(orderId);
  if (it == orders_.end()) {
    return OrderState::kRejected;
  }
  return it->second.state;
}

}  // namespace futu_trader
