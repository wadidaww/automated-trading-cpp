#include "futu_trader/strategy/strategy_factory.hpp"

#include "futu_trader/strategy/strategies.hpp"

namespace futu_trader::strategy {

std::unique_ptr<IStrategy> makeStrategy(const StrategySpec& spec,
                                        const std::vector<QuoteEvent>* lookaheadData) {
  if (spec.name == "meanrev") {
    return std::make_unique<MeanReversion>(
        MeanReversion::Params{spec.symbol, spec.window, spec.entryZx10, spec.exitZx10, spec.qty});
  }
  if (spec.name == "buyhold") {
    return std::make_unique<BuyAndHold>(spec.symbol, spec.qty);
  }
  if (spec.name == "random") {
    return std::make_unique<RandomTrader>(spec.symbol, spec.qty, spec.everyN);
  }
  if (spec.name == "maker") {
    return std::make_unique<PassiveMaker>(
        PassiveMaker::Params{spec.symbol, spec.qty, spec.cancelAfterQuotes});
  }
  if (spec.name == "peeker") {
    return std::make_unique<FuturePeeker>(spec.symbol, spec.qty, lookaheadData);
  }
  return nullptr;
}

}  // namespace futu_trader::strategy
