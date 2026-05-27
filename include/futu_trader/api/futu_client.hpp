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
  int heartbeat_interval_s{10};
  int reconnect_max_attempts{5};
};

/** Futu OpenAPI client abstraction. */
class FutuClient {
 public:
  explicit FutuClient(FutuClientConfig config);

  bool Connect();
  void Disconnect();
  bool IsConnected() const;

  std::string InitConnect();
  std::string PlaceOrder(const Order& order);
  bool ModifyOrder(const std::string& order_id, Money new_price_minor, std::int64_t new_quantity);
  bool CancelOrder(const std::string& order_id);
  std::vector<Order> GetOrderList() const;

  std::optional<Tick> GetBasicQot(const std::string& symbol) const;
  std::vector<Tick> GetKL(const std::string& symbol, std::size_t bars) const;
  bool Subscribe(const std::string& symbol);

 private:
  FutuClientConfig config_;
  mutable std::mutex mu_;
  bool connected_{false};
  std::unordered_map<std::string, Order> orders_;
  std::unordered_map<std::string, Tick> last_ticks_;
};

}  // namespace futu_trader
