#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "futu_trader/api/futu_client.hpp"
#include "futu_trader/core/types.hpp"

namespace futu_trader {

class OrderManager {
 public:
  explicit OrderManager(FutuClient& client);

  /**
   * Submits an order. Returns false on a duplicate idempotency key or if the venue rejects it.
   * A rejected order is recorded as kRejected and its idempotency key is released so the caller
   * may retry.
   */
  bool submit(Order order);
  bool cancel(const std::string& orderId);
  bool modify(const std::string& orderId, Money newPriceMinor, std::int64_t newQuantity);
  OrderState state(const std::string& orderId) const;

  /** Orders that are submitted or partially filled (still working at the venue). */
  std::size_t openOrderCount() const;

 private:
  FutuClient& client_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Order> orders_;
  std::unordered_map<std::string, std::string> dedupe_;  // committed keys
  std::unordered_set<std::string> inFlightKeys_;         // keys reserved while placing
};

}  // namespace futu_trader
