#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "futu_trader/backtest/synthetic.hpp"
#include "futu_trader/core/clock.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/portfolio/position_book.hpp"
#include "futu_trader/strategy/strategy.hpp"

namespace futu_trader::strategy {

/**
 * The StrategyContext used both live and in backtests: every order goes through the OMS (risk,
 * rate limit, idempotency), positions come from our own book, and "now" is the injected clock.
 * One implementation for both modes is what makes a backtest evidence about live behaviour.
 *
 * Quote freshness: the OMS judges a quote's age on OUR clock, so the receive time is stamped here
 * (observe()) rather than taken from the quote's own timestamp, which in live trading is the
 * exchange's wall clock and unrelated to our monotonic clock.
 */
class OmsContext final : public StrategyContext {
 public:
  /** Called after every submit with what was asked and what the OMS decided. */
  using SubmitObserver =
      std::function<void(std::int64_t nowNs, const std::string& symbol, Side side, std::int64_t qty,
                         Money priceMills, const oms::SubmitResult& result)>;

  OmsContext(oms::Oms& oms, const portfolio::PositionBook& book, const Clock& clock,
             std::uint64_t seed)
      : oms_(oms), book_(book), clock_(clock), rng_(seed) {}

  /** Records the latest quote for a symbol and when we received it. Call before the strategy. */
  void observe(const QuoteEvent& quote);
  void setSubmitObserver(SubmitObserver observer) { observer_ = std::move(observer); }

  std::int64_t nowNs() const override { return clock_.nowNs(); }
  std::int64_t position(const std::string& symbol) const override { return book_.qty(symbol); }
  bool hasLiveOrder(const std::string& symbol) const override { return oms_.hasLiveOrder(symbol); }
  std::vector<std::string> liveOrderIds(const std::string& symbol) const override {
    return oms_.liveOrderIds(symbol);
  }
  oms::SubmitResult submit(const std::string& symbol, Side side, std::int64_t qty,
                           Money priceMills) override;
  Result<bool> cancel(const std::string& clOrdId) override { return oms_.cancel(clOrdId); }
  std::uint64_t random() override { return rng_.next(); }

 private:
  struct Seen {
    Money mid{0};
    std::int64_t receivedNs{0};
  };
  oms::Oms& oms_;
  const portfolio::PositionBook& book_;
  const Clock& clock_;
  backtest::Rng rng_;
  std::map<std::string, Seen> seen_;
  SubmitObserver observer_;
  std::uint64_t counter_{0};
};

}  // namespace futu_trader::strategy
