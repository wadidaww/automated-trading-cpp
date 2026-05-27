#include "futu_trader/api/futu_client.hpp"

#include <chrono>

namespace futu_trader {

FutuClient::FutuClient(FutuClientConfig config) : config_(std::move(config)) {}

bool FutuClient::connect() {
  std::scoped_lock lock(mu_);
  connected_ = true;
  return true;
}

void FutuClient::disconnect() {
  std::scoped_lock lock(mu_);
  connected_ = false;
}

bool FutuClient::isConnected() const {
  std::scoped_lock lock(mu_);
  return connected_;
}

std::string FutuClient::initConnect() { return isConnected() ? "connected" : "disconnected"; }

std::string FutuClient::placeOrder(const Order& order) {
  std::scoped_lock lock(mu_);
  orders_[order.orderId] = order;
  orders_[order.orderId].state = OrderState::kSubmitted;
  return order.orderId;
}

bool FutuClient::modifyOrder(const std::string& orderId, Money newPriceMinor,
                             std::int64_t newQuantity) {
  std::scoped_lock lock(mu_);
  auto it = orders_.find(orderId);
  if (it == orders_.end()) {
    return false;
  }
  it->second.limitPriceMinor = newPriceMinor;
  it->second.quantity = newQuantity;
  return true;
}

bool FutuClient::cancelOrder(const std::string& orderId) {
  std::scoped_lock lock(mu_);
  auto it = orders_.find(orderId);
  if (it == orders_.end()) {
    return false;
  }
  it->second.state = OrderState::kCancelled;
  return true;
}

std::vector<Order> FutuClient::getOrderList() const {
  std::scoped_lock lock(mu_);
  std::vector<Order> out;
  out.reserve(orders_.size());
  for (const auto& [_, order] : orders_) {
    out.push_back(order);
  }
  return out;
}

std::optional<Tick> FutuClient::getBasicQot(const std::string& symbol) const {
  std::scoped_lock lock(mu_);
  auto it = last_ticks_.find(symbol);
  if (it == last_ticks_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<Tick> FutuClient::getKl(const std::string& symbol, std::size_t bars) const {
  std::vector<Tick> ticks;
  ticks.reserve(bars);
  for (std::size_t i = 0; i < bars; ++i) {
    ticks.push_back(Tick{symbol,
                         static_cast<Money>(10000 + static_cast<Money>(i)),
                         100,
                         std::chrono::system_clock::now()});
  }
  return ticks;
}

bool FutuClient::subscribe(const std::string& symbol) {
  std::scoped_lock lock(mu_);
  last_ticks_[symbol] = Tick{symbol, 10000, 100, std::chrono::system_clock::now()};
  return true;
}

}  // namespace futu_trader
