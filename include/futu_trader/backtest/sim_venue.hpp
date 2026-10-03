#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "futu_trader/core/clock.hpp"
#include "futu_trader/instrument/hk_rules.hpp"
#include "futu_trader/market/quote.hpp"
#include "futu_trader/oms/venue.hpp"
#include "futu_trader/portfolio/fees.hpp"
#include "futu_trader/portfolio/position_book.hpp"

namespace futu_trader::backtest {

struct SimVenueConfig {
  std::int64_t latencyNs{50'000'000};        // order sent -> live at the market
  std::int64_t cancelLatencyNs{50'000'000};  // cancel sent -> effective (a fill can win the race)
  Money initialCashMills{1'000'000'000};     // 1,000,000 HKD
  portfolio::HkFeeSchedule fees;
  bool allowShort{false};
  /**
   * A resting order fills only when the market trades strictly THROUGH its price (ask < buy
   * limit, bid > sell limit), and then at its own limit price. Quote data has no queue
   * information, so touching the price is not evidence of a fill; this is the conservative choice.
   */
  bool passiveNeedsTradeThrough{true};
  /**
   * How to treat a quote whose displayed size is 0 (unknown). True: unlimited liquidity (handy for
   * hand-made tests). False: no fill is possible at that quote, because unknown liquidity is not
   * evidence of any. Real backtests should use false; defaultBacktestConfig() does.
   */
  bool sizeZeroMeansUnlimited{true};
};

/**
 * A deterministic simulated broker implementing the same IVenue interface as OpenD, so the real
 * OMS, risk chain and position book run unchanged in backtests.
 *
 * Fill model (quote data only):
 *  - Orders reach the market `latencyNs` after being placed, and are matched against the market as
 *    it is THEN, not as it was when the strategy decided.
 *  - A marketable order (buy limit >= ask, sell limit <= bid) fills at the touch price, limited to
 *    the displayed size still unconsumed by earlier fills at that touch; any remainder rests.
 *  - Resting orders fill on later quotes per `passiveNeedsTradeThrough`, at their limit price.
 *  - Like a real broker it rejects bad ticks/lots, unaffordable buys and unsellable shares.
 *
 * Not modelled: queue position, market impact, hidden liquidity, halts, auctions, corporate
 * actions. Results from it are optimistic about liquidity for large sizes.
 * Backtest-only: it advances the ManualClock, so it is single-threaded.
 */
class SimVenue final : public oms::IVenue {
 public:
  using OrderSink = std::function<void(const opend::BrokerOrder&)>;
  using FillSink = std::function<void(const opend::BrokerFill&)>;

  SimVenue(SimVenueConfig config, const instrument::InstrumentTable& instruments,
           ManualClock& clock);

  /** Where order updates and fills are delivered (normally Oms::onOrderUpdate / onFill). */
  void setSinks(OrderSink orderSink, FillSink fillSink);

  // oms::IVenue
  Result<opend::PlacedOrder> place(const opend::PlaceOrderRequest& request) override;
  Result<bool> cancel(std::uint64_t venueOrderId) override;
  Result<std::vector<opend::BrokerOrder>> listOrders() override;
  Result<std::vector<opend::BrokerFill>> listFills() override;
  Result<std::vector<opend::PositionInfo>> listPositions() override;
  Result<opend::FundsInfo> funds() override;

  /** Applies a new market quote (may fill resting orders). */
  void onQuote(const QuoteEvent& quote);
  /** Runs every scheduled action (activations, cancels) due at or before `nowNs`. */
  void advance(std::int64_t nowNs);
  /** Time of the next scheduled action, or -1 if none. */
  std::int64_t nextActionNs() const;

  // Ground truth, for metrics and for cross-checking the OMS.
  Money cash() const;
  std::int64_t position(const std::string& symbol) const;
  /** Cash plus positions marked at `marks` (missing marks fall back to cost). */
  Money equity(const std::map<std::string, Money>& marks) const;
  const std::vector<opend::BrokerFill>& fills() const { return fills_; }
  Money totalFees() const { return truth_.totalFees(); }
  const portfolio::PositionBook& book() const { return truth_; }
  std::size_t orderCount() const { return orders_.size(); }

 private:
  struct SimOrder {
    opend::BrokerOrder pub;
    std::int64_t remaining{0};
    bool active{false};
    bool terminal{false};
    bool sellShort{false};
    bool aggressive{false};  // arrived marketable: its remainder keeps taking the touch price
  };
  enum class ActionKind : std::uint8_t { kActivate, kCancel };
  struct Action {
    ActionKind kind{ActionKind::kActivate};
    std::uint64_t orderId{0};
  };

  Money reservedBuyMills() const;
  std::int64_t reservedSellQty(const std::string& symbol) const;
  void schedule(std::int64_t atNs, ActionKind kind, std::uint64_t orderId);
  void tryMatch(SimOrder& order, const QuoteEvent& quote, bool arriving);
  void fill(SimOrder& order, std::int64_t qty, Money price);
  void emitOrder(const SimOrder& order);
  void closeOrder(SimOrder& order);  // marks terminal and drops it from the open index

  SimVenueConfig config_;
  const instrument::InstrumentTable& instruments_;
  ManualClock& clock_;
  OrderSink orderSink_;
  FillSink fillSink_;

  std::map<std::uint64_t, SimOrder> orders_;  // ordered by id == creation order
  // Orders that are not yet terminal, in creation order. Reservations and quote matching only ever
  // concern these, so they must not walk every order placed so far: a long run places thousands.
  // `orders_` is node-based and never erased from, so these pointers stay valid.
  std::map<std::uint64_t, SimOrder*> open_;
  std::map<std::pair<std::int64_t, std::uint64_t>, Action> schedule_;
  std::map<std::string, QuoteEvent> quotes_;
  // Displayed size already consumed by our own fills at the current touch. A refreshed quote with
  // the same price and size is not new liquidity, so consumption survives it.
  struct Consumed {
    std::int64_t ask{0};
    std::int64_t bid{0};
  };
  std::map<std::string, Consumed> consumed_;
  std::vector<opend::BrokerFill> fills_;
  portfolio::PositionBook truth_;
  std::uint64_t nextOrderId_{1000};
  std::uint64_t nextFillId_{1};
  std::uint64_t nextSeq_{1};
};

}  // namespace futu_trader::backtest
