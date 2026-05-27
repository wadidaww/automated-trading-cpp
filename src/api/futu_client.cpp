#include "futu_trader/api/futu_client.hpp"

#include <chrono>

namespace futu_trader {

FutuClient::FutuClient(FutuClientConfig config) : config_(std::move(config)) {}

bool FutuClient::Connect() {
  std::scoped_lock lock(mu_);
  connected_ = true;
  return true;
}

void FutuClient::Disconnect() {
  std::scoped_lock lock(mu_);
  connected_ = false;
}

bool FutuClient::IsConnected() const {
  std::scoped_lock lock(mu_);
  return connected_;
}

std::string FutuClient::InitConnect() { return IsConnected() ? "connected" : "disconnected"; }

std::string FutuClient::PlaceOrder(const Order& order) {
  std::scoped_lock lock(mu_);
  orders_[order.order_id] = order;
  orders_[order.order_id].state = OrderState::kSubmitted;
  return order.order_id;
}

bool FutuClient::ModifyOrder(const std::string& order_id, Money new_price_minor,
                             std::int64_t new_quantity) {
  std::scoped_lock lock(mu_);
  auto it = orders_.find(order_id);
  if (it == orders_.end()) {
    return false;
  }
  it->second.limit_price_minor = new_price_minor;
  it->second.quantity = new_quantity;
  return true;
}

bool FutuClient::CancelOrder(const std::string& order_id) {
  std::scoped_lock lock(mu_);
  auto it = orders_.find(order_id);
  if (it == orders_.end()) {
    return false;
  }
  it->second.state = OrderState::kCancelled;
  return true;
}

std::vector<Order> FutuClient::GetOrderList() const {
  std::scoped_lock lock(mu_);
  std::vector<Order> out;
  out.reserve(orders_.size());
  for (const auto& [_, order] : orders_) {
    out.push_back(order);
  }
  return out;
}

std::optional<Tick> FutuClient::GetBasicQot(const std::string& symbol) const {
  std::scoped_lock lock(mu_);
  auto it = last_ticks_.find(symbol);
  if (it == last_ticks_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<Tick> FutuClient::GetKL(const std::string& symbol, std::size_t bars) const {
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

bool FutuClient::Subscribe(const std::string& symbol) {
  std::scoped_lock lock(mu_);
  last_ticks_[symbol] = Tick{symbol, 10000, 100, std::chrono::system_clock::now()};
  return true;
}

}  // namespace futu_trader
