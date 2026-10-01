#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "futu_trader/strategy/strategy.hpp"

namespace futu_trader::strategy {

/** Buys `qty` at the first quote and holds. The baseline every strategy must be compared with. */
class BuyAndHold final : public IStrategy {
 public:
  BuyAndHold(std::string symbol, std::int64_t qty) : symbol_(std::move(symbol)), qty_(qty) {}
  void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override;

 private:
  std::string symbol_;
  std::int64_t qty_;
  bool done_{false};
};

/**
 * Long-only mean reversion on the mid price: buy `qty` when the mid is more than `entryZ` rolling
 * standard deviations below its `window`-quote mean, sell everything once it is back to within
 * `exitZ` of the mean. The z-score test is done in exact integer arithmetic, so results are
 * bit-identical across compilers and platforms (thresholds are given in tenths: 20 == 2.0).
 * It trades one order at a time and only ever uses data up to the current quote.
 */
class MeanReversion final : public IStrategy {
 public:
  struct Params {
    std::string symbol{"00700"};
    std::size_t window{60};
    int entryZx10{20};
    int exitZx10{0};
    std::int64_t qty{100};
  };
  explicit MeanReversion(Params params) : params_(std::move(params)) {}
  void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override;

 private:
  bool zBelow(Money mid, int kx10) const;  // z < -k, exact
  Params params_;
  std::deque<Money> mids_;
};

/**
 * Reference passive strategy that exercises the order-lifecycle paths a taker strategy never
 * touches: it rests a buy AT THE BID and, once long, a sell AT THE ASK, and cancels any order that
 * has not filled within `cancelAfterQuotes` quotes. Resting orders fill only when the market
 * trades through them, so it sees partial fills, unfilled orders, cancels and cancel/fill races.
 * It has no claimed edge: it exists to drive the simulator through those paths.
 */
class PassiveMaker final : public IStrategy {
 public:
  struct Params {
    std::string symbol{"00700"};
    std::int64_t qty{300};
    std::size_t cancelAfterQuotes{8};
  };
  explicit PassiveMaker(Params params) : params_(std::move(params)) {}
  void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override;

 private:
  Params params_;
  std::size_t quoteCount_{0};
  std::size_t placedAt_{0};
};

/**
 * CANARY, zero edge: every `everyN` quotes it flips a seeded coin and enters or leaves a position
 * at the touch. Across many seeds it must lose about the trading costs; a backtest that shows it
 * making money is broken.
 */
class RandomTrader final : public IStrategy {
 public:
  RandomTrader(std::string symbol, std::int64_t qty, std::size_t everyN)
      : symbol_(std::move(symbol)), qty_(qty), everyN_(everyN) {}
  void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override;

 private:
  std::string symbol_;
  std::int64_t qty_;
  std::size_t everyN_;
  std::size_t seen_{0};
};

/**
 * CANARY, cheats: it is handed the whole future and trades on the NEXT quote. It can only be
 * written by breaking the strategy API, and it exists to prove that detectLookahead() catches
 * exactly this. Never use it for anything else.
 */
class FuturePeeker final : public IStrategy {
 public:
  FuturePeeker(std::string symbol, std::int64_t qty, const std::vector<QuoteEvent>* future)
      : symbol_(std::move(symbol)), qty_(qty), future_(future) {}
  void onQuote(const QuoteEvent& quote, StrategyContext& ctx) override;

 private:
  std::string symbol_;
  std::int64_t qty_;
  const std::vector<QuoteEvent>* future_;
  std::size_t index_{0};
};

}  // namespace futu_trader::strategy
