#include "futu_trader/execution/position_tracker.hpp"

namespace futu_trader {

void PositionTracker::OnFill(const std::string& symbol, std::int64_t quantity, Money) {
  std::scoped_lock lock(mu_);
  qty_by_symbol_[symbol] += quantity;
}

Money PositionTracker::PositionNotional(const std::string& symbol, Money mark_price_minor) const {
  std::scoped_lock lock(mu_);
  const auto it = qty_by_symbol_.find(symbol);
  if (it == qty_by_symbol_.end()) {
    return 0;
  }
  return static_cast<Money>(it->second) * mark_price_minor;
}

std::unordered_map<std::string, std::int64_t> PositionTracker::Quantities() const {
  std::scoped_lock lock(mu_);
  return qty_by_symbol_;
}

}  // namespace futu_trader
