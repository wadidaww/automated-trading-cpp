#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "futu_trader/core/types.hpp"

namespace futu_trader {

struct FutuClientConfig {
  std::string host{"127.0.0.1"};
  int port{11111};
  int heartbeatIntervalSeconds{10};
  int reconnectMaxAttempts{5};
};

/** Futu OpenAPI client abstraction. */
class FutuClient {
 public:
  explicit FutuClient(FutuClientConfig config);

  bool connect();
  void disconnect();
  bool isConnected() const;

  std::string initConnect();
  std::string placeOrder(const Order& order);
  bool modifyOrder(const std::string& orderId, Money newPriceMinor, std::int64_t newQuantity);
  bool cancelOrder(const std::string& orderId);
  std::vector<Order> getOrderList() const;

  std::optional<Tick> getBasicQot(const std::string& symbol) const;
  std::vector<Tick> getKl(const std::string& symbol, std::size_t bars) const;
  bool subscribe(const std::string& symbol);

 private:
  FutuClientConfig config_;
  mutable std::mutex mu_;
  bool connected_{false};
  std::unordered_map<std::string, Order> orders_;
  std::unordered_map<std::string, Tick> last_ticks_;
};

}  // namespace futu_trader
