#include "futu_trader/execution/position_tracker.hpp"

namespace futu_trader {

void PositionTracker::onFill(const std::string& symbol, std::int64_t quantity,
                             Money /*fillPriceMinor*/) {
  std::scoped_lock lock(mu_);
  qtyBySymbol_[symbol] += quantity;
}

Money PositionTracker::positionNotional(const std::string& symbol, Money markPriceMinor) const {
  std::scoped_lock lock(mu_);
  const auto it = qtyBySymbol_.find(symbol);
  if (it == qtyBySymbol_.end()) {
    return 0;
  }
  return static_cast<Money>(it->second) * markPriceMinor;
}

std::unordered_map<std::string, std::int64_t> PositionTracker::quantities() const {
  std::scoped_lock lock(mu_);
  return qtyBySymbol_;
}

}  // namespace futu_trader
