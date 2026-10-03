#include "futu_trader/portfolio/position_book.hpp"

#include <algorithm>
#include <cstdlib>

namespace futu_trader::portfolio {

namespace {

__extension__ using Int128 = __int128;

Error overflow() { return Error{ErrorCode::kInvalidArg, "arithmetic overflow"}; }

bool fits(Int128 value) { return value >= INT64_MIN && value <= INT64_MAX; }

// Round-half-away-from-zero division of a non-negative numerator.
Int128 divRound(Int128 num, Int128 den) { return (num + (den / 2)) / den; }

}  // namespace

Result<bool> PositionBook::applyFill(const FillRecord& fill) {
  if (fill.qty <= 0 || fill.price <= 0 || fill.fee < 0 || fill.symbol.empty() ||
      fill.fillId.empty()) {
    return Error{ErrorCode::kInvalidArg, "invalid fill"};
  }
  std::scoped_lock lock(mu_);
  if (seenFills_.contains(fill.fillId)) {
    return false;
  }

  const Int128 notional = static_cast<Int128>(fill.price) * fill.qty;
  if (!fits(notional)) {
    return overflow();
  }
  const std::int64_t signedQty = fill.side == Side::kBuy ? fill.qty : -fill.qty;
  Entry next = entries_[fill.symbol];  // copy: only commit if everything succeeds

  const std::int64_t held = next.qty;
  const bool reduces = held != 0 && ((held > 0) != (signedQty > 0));
  std::int64_t closeQty = 0;
  std::int64_t openQty = fill.qty;
  if (reduces) {
    closeQty = std::min<std::int64_t>(fill.qty, std::llabs(held));
    openQty = fill.qty - closeQty;
  }

  if (closeQty > 0) {
    const std::int64_t heldAbs = std::llabs(held);
    // Release cost proportionally; releasing everything when fully closed leaves no residue.
    Int128 costReleased = next.costBasis;
    if (closeQty < heldAbs) {
      costReleased = divRound(static_cast<Int128>(next.costBasis) * closeQty, heldAbs);
    }
    const Int128 proceeds = static_cast<Int128>(fill.price) * closeQty;
    // Long closed by a sell: proceeds - cost. Short closed by a buy: cost - proceeds.
    const Int128 pnl = held > 0 ? proceeds - costReleased : costReleased - proceeds;
    const Int128 newRealized = static_cast<Int128>(next.realized) + pnl;
    if (!fits(newRealized)) {
      return overflow();
    }
    next.realized = static_cast<Money>(newRealized);
    next.costBasis = static_cast<Money>(static_cast<Int128>(next.costBasis) - costReleased);
    next.qty += held > 0 ? -closeQty : closeQty;
  }
  if (openQty > 0) {
    const Int128 newBasis =
        static_cast<Int128>(next.costBasis) + (static_cast<Int128>(fill.price) * openQty);
    if (!fits(newBasis)) {
      return overflow();
    }
    next.costBasis = static_cast<Money>(newBasis);
    next.qty += fill.side == Side::kBuy ? openQty : -openQty;
  }

  const Int128 cashFlow = (fill.side == Side::kBuy ? -notional : notional) - fill.fee;
  const Int128 newCash = static_cast<Int128>(cash_) + cashFlow;
  const Int128 newFees = static_cast<Int128>(fees_) + fill.fee;
  if (!fits(newCash) || !fits(newFees)) {
    return overflow();
  }

  entries_[fill.symbol] = next;
  cash_ = static_cast<Money>(newCash);
  fees_ = static_cast<Money>(newFees);
  seenFills_.insert(fill.fillId);
  return true;
}

Result<bool> PositionBook::seedPosition(const std::string& symbol, std::int64_t qty,
                                        Money costPriceMills) {
  if (symbol.empty() || qty == 0 || costPriceMills <= 0 || qty == INT64_MIN) {
    return Error{ErrorCode::kInvalidArg, "invalid seed position"};
  }
  const Int128 basis = static_cast<Int128>(costPriceMills) * std::llabs(qty);
  if (!fits(basis)) {
    return overflow();
  }
  std::scoped_lock lock(mu_);
  Entry& entry = entries_[symbol];
  if (entry.qty != 0) {
    return Error{ErrorCode::kInvalidArg, "position already open for " + symbol};
  }
  entry.qty = qty;
  entry.costBasis = static_cast<Money>(basis);
  return true;
}

void PositionBook::reserveFills(std::size_t n) {
  std::scoped_lock lock(mu_);
  seenFills_.reserve(n);
}

void PositionBook::markFillSeen(const std::string& fillId) {
  std::scoped_lock lock(mu_);
  seenFills_.insert(fillId);
}

std::int64_t PositionBook::qty(const std::string& symbol) const {
  std::scoped_lock lock(mu_);
  const auto found = entries_.find(symbol);
  return found == entries_.end() ? 0 : found->second.qty;
}

PositionSnapshot PositionBook::snapshot(const std::string& symbol) const {
  std::scoped_lock lock(mu_);
  const auto found = entries_.find(symbol);
  if (found == entries_.end()) {
    return {symbol, 0, 0, 0};
  }
  return {symbol, found->second.qty, found->second.costBasis, found->second.realized};
}

std::vector<PositionSnapshot> PositionBook::all() const {
  std::scoped_lock lock(mu_);
  std::vector<PositionSnapshot> out;
  out.reserve(entries_.size());
  for (const auto& [symbol, entry] : entries_) {
    out.push_back({symbol, entry.qty, entry.costBasis, entry.realized});
  }
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.symbol < b.symbol; });  // deterministic
  return out;
}

Result<Money> PositionBook::exposure(const std::string& symbol, Money mark) const {
  const std::int64_t held = qty(symbol);
  const Int128 value = static_cast<Int128>(held) * mark;
  if (!fits(value)) {
    return overflow();
  }
  return static_cast<Money>(value);
}

Result<Money> PositionBook::grossExposure(
    const std::unordered_map<std::string, Money>& marks) const {
  Int128 total = 0;
  for (const auto& position : all()) {
    if (position.qty == 0) {
      continue;
    }
    const auto found = marks.find(position.symbol);
    if (found == marks.end()) {
      return Error{ErrorCode::kInvalidArg, "no mark price for " + position.symbol};
    }
    const Int128 value = static_cast<Int128>(std::llabs(position.qty)) * found->second;
    total += value;
    if (!fits(total)) {
      return overflow();
    }
  }
  return static_cast<Money>(total);
}

Result<Money> PositionBook::unrealizedPnl(const std::string& symbol, Money mark) const {
  const PositionSnapshot pos = snapshot(symbol);
  if (pos.qty == 0) {
    return Money{0};
  }
  const Int128 value = static_cast<Int128>(std::llabs(pos.qty)) * mark;
  const Int128 pnl = pos.qty > 0 ? value - pos.costBasis : pos.costBasis - value;
  if (!fits(pnl)) {
    return overflow();
  }
  return static_cast<Money>(pnl);
}

Money PositionBook::totalFees() const {
  std::scoped_lock lock(mu_);
  return fees_;
}

Money PositionBook::totalRealizedPnl() const {
  std::scoped_lock lock(mu_);
  Int128 total = 0;
  for (const auto& [symbol, entry] : entries_) {
    total += entry.realized;
  }
  // Saturate rather than overflow: a wrapped P&L would defeat the daily-loss limit.
  if (total > INT64_MAX) {
    return INT64_MAX;
  }
  if (total < INT64_MIN) {
    return INT64_MIN;
  }
  return static_cast<Money>(total);
}

Money PositionBook::cashDelta() const {
  std::scoped_lock lock(mu_);
  return cash_;
}

}  // namespace futu_trader::portfolio
