#pragma once

#include <mutex>
#include <string>
#include <unordered_map>

#include "futu_trader/core/types.hpp"

namespace futu_trader {

class PositionTracker {
 public:
  void onFill(const std::string& symbol, std::int64_t quantity, Money fillPriceMinor);
  Money positionNotional(const std::string& symbol, Money markPriceMinor) const;
  std::unordered_map<std::string, std::int64_t> quantities() const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::int64_t> qtyBySymbol_;
};

}  // namespace futu_trader
