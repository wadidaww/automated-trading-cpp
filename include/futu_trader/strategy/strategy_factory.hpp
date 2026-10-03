#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "futu_trader/market/quote.hpp"
#include "futu_trader/strategy/strategy.hpp"

namespace futu_trader::strategy {

/** Everything any reference strategy can be configured with; each one reads what it needs. */
struct StrategySpec {
  std::string name;  // meanrev | buyhold | random | maker | peeker
  std::string symbol;
  std::int64_t qty{100};
  std::size_t window{60};            // meanrev
  int entryZx10{20};                 // meanrev
  int exitZx10{0};                   // meanrev
  std::size_t cancelAfterQuotes{8};  // maker
  std::size_t everyN{25};            // random
};

/**
 * One place that knows how to build a strategy from its name, shared by the live process and the
 * backtest CLI so both construct them identically. Returns nullptr for an unknown name.
 * `lookaheadData` is only used by the "peeker" canary (it cheats on purpose; see strategies.hpp).
 */
std::unique_ptr<IStrategy> makeStrategy(const StrategySpec& spec,
                                        const std::vector<QuoteEvent>* lookaheadData = nullptr);

}  // namespace futu_trader::strategy
