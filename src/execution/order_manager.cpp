#include "futu_trader/execution/order_manager.hpp"

namespace futu_trader {

OrderManager::OrderManager(FutuClient& client) : client_(client) {}

bool OrderManager::submit(Order order) {
  const bool hasKey = !order.idempotencyKey.empty();
  {
    std::scoped_lock lock(mu_);
    if (hasKey &&
        (dedupe_.contains(order.idempotencyKey) || inFlightKeys_.contains(order.idempotencyKey))) {
      return false;
    }
    if (hasKey) {
      inFlightKeys_.insert(order.idempotencyKey);
    }
  }

  // The network call runs without holding the lock so a slow gateway cannot block other callers.
  const std::string venueOrderId = client_.placeOrder(order);
  const bool accepted = !venueOrderId.empty();

  std::scoped_lock lock(mu_);
  if (hasKey) {
    inFlightKeys_.erase(order.idempotencyKey);
  }
  order.state = accepted ? OrderState::kSubmitted : OrderState::kRejected;
  if (accepted && hasKey) {
    dedupe_[order.idempotencyKey] = order.orderId;
  }
  orders_[order.orderId] = order;
  return accepted;
}

bool OrderManager::cancel(const std::string& orderId) {
  {
    std::scoped_lock lock(mu_);
    if (!orders_.contains(orderId)) {
      return false;
    }
  }
  if (!client_.cancelOrder(orderId)) {
    return false;
  }
  std::scoped_lock lock(mu_);
  orders_[orderId].state = OrderState::kCancelled;
  return true;
}

bool OrderManager::modify(const std::string& orderId, Money newPriceMinor,
                          std::int64_t newQuantity) {
  {
    std::scoped_lock lock(mu_);
    if (!orders_.contains(orderId)) {
      return false;
    }
  }
  if (!client_.modifyOrder(orderId, newPriceMinor, newQuantity)) {
    return false;
  }
  std::scoped_lock lock(mu_);
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

std::size_t OrderManager::openOrderCount() const {
  std::scoped_lock lock(mu_);
  std::size_t count = 0;
  for (const auto& [_, order] : orders_) {
    if (order.state == OrderState::kSubmitted || order.state == OrderState::kPartialFill) {
      ++count;
    }
  }
  return count;
}

}  // namespace futu_trader
