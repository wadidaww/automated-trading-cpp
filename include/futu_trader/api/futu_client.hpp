#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

#include "futu_trader/core/types.hpp"

namespace futu_trader {

namespace futuOpenApiCommand {
inline constexpr int initConnect = 1001;
inline constexpr int unlockTrade = 2005;
inline constexpr int placeOrder = 2201;
inline constexpr int cancelOrder = 2202;
inline constexpr int modifyOrder = 2205;
inline constexpr int getOrderList = 2221;
inline constexpr int subscribe = 3001;
inline constexpr int getSecuritySnapshot = 3007;
inline constexpr int getKl = 3100;
}  // namespace futuOpenApiCommand

struct FutuOpenApiResult {
  bool success{false};
  std::string payload;
};

using FutuOpenApiInvoker = std::function<FutuOpenApiResult(int command, const std::string& payload)>;

struct FutuClientConfig {
  std::string host{"127.0.0.1"};
  int port{11111};
  int heartbeatIntervalSeconds{10};
  int reconnectMaxAttempts{5};
  std::string tradePasswordMd5;
  bool requireUnlockTrade{false};
  FutuOpenApiInvoker apiInvoker;
};

/** Futu OpenAPI client abstraction. */
class FutuClient {
 public:
  explicit FutuClient(FutuClientConfig config);

  bool connect();
  void disconnect();
  bool isConnected() const;

  std::string initConnect();
  bool unlockTrade();
  std::string placeOrder(const Order& order);
  bool modifyOrder(const std::string& orderId, Money newPriceMinor, std::int64_t newQuantity);
  bool cancelOrder(const std::string& orderId);
  std::vector<Order> getOrderList() const;

  std::optional<Tick> getBasicQot(const std::string& symbol) const;
  std::vector<Tick> getKl(const std::string& symbol, std::size_t bars) const;
  bool subscribe(const std::string& symbol);

 private:
  FutuOpenApiResult callOpenApi(int command, const std::string& payload) const;

  FutuClientConfig config_;
  mutable std::mutex mu_;
  bool connected_{false};
  bool tradeUnlocked_{false};
  std::unordered_map<std::string, Order> orders_;
  std::unordered_map<std::string, Tick> last_ticks_;
};

}  // namespace futu_trader
