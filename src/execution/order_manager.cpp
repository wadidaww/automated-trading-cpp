#include "futu_trader/execution/order_manager.hpp"

namespace futu_trader {

OrderManager::OrderManager(FutuClient& client) : client_(client) {}

bool OrderManager::Submit(Order order) {
  std::scoped_lock lock(mu_);
  if (!order.idempotency_key.empty() && dedupe_.contains(order.idempotency_key)) {
    return false;
  }
  client_.PlaceOrder(order);
  order.state = OrderState::kSubmitted;
  if (!order.idempotency_key.empty()) {
    dedupe_[order.idempotency_key] = order.order_id;
  }
  orders_[order.order_id] = order;
  return true;
}

bool OrderManager::Cancel(const std::string& order_id) {
  std::scoped_lock lock(mu_);
  if (!orders_.contains(order_id)) {
    return false;
  }
  if (!client_.CancelOrder(order_id)) {
    return false;
  }
  orders_[order_id].state = OrderState::kCancelled;
  return true;
}

bool OrderManager::Modify(const std::string& order_id, Money new_price_minor,
                          std::int64_t new_quantity) {
  std::scoped_lock lock(mu_);
  if (!orders_.contains(order_id)) {
    return false;
  }
  if (!client_.ModifyOrder(order_id, new_price_minor, new_quantity)) {
    return false;
  }
  orders_[order_id].limit_price_minor = new_price_minor;
  orders_[order_id].quantity = new_quantity;
  return true;
}

OrderState OrderManager::State(const std::string& order_id) const {
  std::scoped_lock lock(mu_);
  auto it = orders_.find(order_id);
  if (it == orders_.end()) {
    return OrderState::kRejected;
  }
  return it->second.state;
}

}  // namespace futu_trader
