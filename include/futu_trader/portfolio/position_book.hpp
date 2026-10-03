#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/core/types.hpp"

namespace futu_trader::portfolio {

struct FillRecord {
  std::string fillId;  // broker fill id; the same id is never applied twice
  std::string symbol;
  Side side{Side::kBuy};
  std::int64_t qty{0};  // positive
  Money price{0};       // mills per share
  Money fee{0};         // mills, >= 0
};

struct PositionSnapshot {
  std::string symbol;
  std::int64_t qty{0};   // signed: long > 0, short < 0
  Money costBasis{0};    // total cost of the open position, mills (always >= 0)
  Money realizedPnl{0};  // gross of fees, mills
};

/**
 * Positions, cost basis, realized P&L, fees and cash from fills.
 *
 * Cost basis is tracked as a total (not an average) so no rounding residue accumulates; when a
 * position is closed the remaining basis is released exactly. A fill that crosses zero is split
 * into a closing part and an opening part at the same price. All arithmetic is overflow-checked.
 * Fills are idempotent by fillId (pushes and reconciliation may deliver the same fill twice).
 */
class PositionBook {
 public:
  /**
   * Returns true if applied, false if it was a duplicate fillId; error on invalid/overflow. A
   * fill without an id is invalid: without one, a push and a reconciliation listing of the same
   * fill would be applied twice.
   */
  Result<bool> applyFill(const FillRecord& fill);

  /**
   * Initialises a position carried over from before this process started (from the broker).
   * Only allowed while the symbol is flat in the book; records no cash flow or P&L.
   */
  Result<bool> seedPosition(const std::string& symbol, std::int64_t qty, Money costPriceMills);

  /** Pre-sizes the duplicate-fill index so it does not rehash mid-session. */
  void reserveFills(std::size_t n);

  /** Marks a fill id as already reflected in the book (e.g. inside a seeded position). */
  void markFillSeen(const std::string& fillId);

  std::int64_t qty(const std::string& symbol) const;
  PositionSnapshot snapshot(const std::string& symbol) const;
  std::vector<PositionSnapshot> all() const;

  /** Signed notional of a position at the given mark (long > 0, short < 0). */
  Result<Money> exposure(const std::string& symbol, Money mark) const;
  /** Gross notional across all symbols; marks maps symbol -> price, missing marks are errors. */
  Result<Money> grossExposure(const std::unordered_map<std::string, Money>& marks) const;
  Result<Money> unrealizedPnl(const std::string& symbol, Money mark) const;

  Money totalFees() const;
  Money totalRealizedPnl() const;
  /** Sum of trade cash flows and fees since start (buys negative). Compare to broker cash deltas.
   */
  Money cashDelta() const;

 private:
  struct Entry {
    std::int64_t qty{0};
    Money costBasis{0};
    Money realized{0};
  };

  mutable std::mutex mu_;
  std::unordered_map<std::string, Entry> entries_;
  std::unordered_set<std::string> seenFills_;
  Money fees_{0};
  Money cash_{0};
};

}  // namespace futu_trader::portfolio
