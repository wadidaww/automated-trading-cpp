#include "futu_trader/api/futu_client.hpp"

#include <chrono>
#include <sstream>

namespace futu_trader {

FutuClient::FutuClient(FutuClientConfig config) : config_(std::move(config)) {}

bool FutuClient::connect() {
  std::scoped_lock lock(mu_);
  if (config_.apiInvoker) {
    const auto result = callOpenApi(futuOpenApiCommand::initConnect, "");
    connected_ = result.success;
  } else {
    connected_ = true;
  }
  tradeUnlocked_ = connected_ && !config_.requireUnlockTrade;
  return connected_;
}

void FutuClient::disconnect() {
  std::scoped_lock lock(mu_);
  connected_ = false;
  tradeUnlocked_ = false;
}

bool FutuClient::isConnected() const {
  std::scoped_lock lock(mu_);
  return connected_;
}

std::string FutuClient::initConnect() {
  std::scoped_lock lock(mu_);
  if (config_.apiInvoker) {
    const auto result = callOpenApi(futuOpenApiCommand::initConnect, "");
    if (!result.payload.empty()) {
      return result.payload;
    }
  }
  return connected_ ? "connected" : "disconnected";
}

bool FutuClient::unlockTrade() {
  std::scoped_lock lock(mu_);
  if (!connected_) {
    return false;
  }
  if (!config_.requireUnlockTrade) {
    tradeUnlocked_ = true;
    return true;
  }
  if (config_.tradePasswordMd5.empty()) {
    tradeUnlocked_ = false;
    return false;
  }
  if (config_.apiInvoker) {
    const auto result = callOpenApi(futuOpenApiCommand::unlockTrade, config_.tradePasswordMd5);
    tradeUnlocked_ = result.success;
    return tradeUnlocked_;
  }
  tradeUnlocked_ = true;
  return true;
}

std::string FutuClient::placeOrder(const Order& order) {
  std::scoped_lock lock(mu_);
  if (!connected_ || (config_.requireUnlockTrade && !tradeUnlocked_)) {
    return "";
  }
  if (config_.apiInvoker) {
    std::ostringstream payload;
    payload << "symbol=" << order.symbol << ";side=" << (order.side == Side::kBuy ? "buy" : "sell")
            << ";qty=" << order.quantity << ";priceMinor=" << order.limitPriceMinor;
    if (!callOpenApi(futuOpenApiCommand::placeOrder, payload.str()).success) {
      return "";
    }
  }
  orders_[order.orderId] = order;
  orders_[order.orderId].state = OrderState::kSubmitted;
  return order.orderId;
}

bool FutuClient::modifyOrder(const std::string& orderId, Money newPriceMinor,
                             std::int64_t newQuantity) {
  std::scoped_lock lock(mu_);
  if (config_.apiInvoker) {
    std::ostringstream payload;
    payload << "orderId=" << orderId << ";priceMinor=" << newPriceMinor << ";qty=" << newQuantity;
    if (!callOpenApi(futuOpenApiCommand::modifyOrder, payload.str()).success) {
      return false;
    }
  }
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
  if (config_.apiInvoker) {
    if (!callOpenApi(futuOpenApiCommand::cancelOrder, orderId).success) {
      return false;
    }
  }
  auto it = orders_.find(orderId);
  if (it == orders_.end()) {
    return false;
  }
  it->second.state = OrderState::kCancelled;
  return true;
}

std::vector<Order> FutuClient::getOrderList() const {
  std::scoped_lock lock(mu_);
  if (config_.apiInvoker) {
    (void)callOpenApi(futuOpenApiCommand::getOrderList, "");
  }
  std::vector<Order> out;
  out.reserve(orders_.size());
  for (const auto& [_, order] : orders_) {
    out.push_back(order);
  }
  return out;
}

std::optional<Tick> FutuClient::getBasicQot(const std::string& symbol) const {
  std::scoped_lock lock(mu_);
  if (config_.apiInvoker) {
    (void)callOpenApi(futuOpenApiCommand::getSecuritySnapshot, symbol);
  }
  auto it = last_ticks_.find(symbol);
  if (it == last_ticks_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<Tick> FutuClient::getKl(const std::string& symbol, std::size_t bars) const {
  FutuOpenApiInvoker invoker;
  {
    std::scoped_lock lock(mu_);
    invoker = config_.apiInvoker;
  }
  if (invoker) {
    std::ostringstream payload;
    payload << "symbol=" << symbol << ";bars=" << bars;
    (void)invoker(futuOpenApiCommand::getKl, payload.str());
  }
  std::vector<Tick> ticks;
  ticks.reserve(bars);
  for (std::size_t i = 0; i < bars; ++i) {
    ticks.push_back(Tick{.symbol = symbol,
                         .priceMinor = static_cast<Money>(10000 + static_cast<Money>(i)),
                         .volume = 100,
                         .timestamp = std::chrono::system_clock::now()});
  }
  return ticks;
}

bool FutuClient::subscribe(const std::string& symbol) {
  std::scoped_lock lock(mu_);
  if (config_.apiInvoker && !callOpenApi(futuOpenApiCommand::subscribe, symbol).success) {
    return false;
  }
  last_ticks_[symbol] = Tick{.symbol = symbol,
                             .priceMinor = 10000,
                             .volume = 100,
                             .timestamp = std::chrono::system_clock::now()};
  return true;
}

FutuOpenApiResult FutuClient::callOpenApi(int command, const std::string& payload) const {
  if (!config_.apiInvoker) {
    return {};
  }
  return config_.apiInvoker(command, payload);
}

}  // namespace futu_trader
