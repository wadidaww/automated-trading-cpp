#include "futu_trader/backtest/synthetic.hpp"

#include <algorithm>

#include "futu_trader/instrument/hk_rules.hpp"

namespace futu_trader::backtest {

namespace {

constexpr Money kMinMid = 250;        // 0.250: keep a spread inside the valid range
constexpr Money kMaxMid = 9'000'000;  // well below the 9,995.000 ceiling

Money snapDown(Money price) {
  const Money tick = instrument::hkEquityTick(price);
  return tick > 0 ? (price / tick) * tick : price;
}

}  // namespace

std::vector<QuoteEvent> generateSyntheticQuotes(const SyntheticConfig& config) {
  std::vector<QuoteEvent> out;
  out.reserve(config.count);
  Rng rng(config.seed);
  const Money anchor = snapDown(config.startMid);
  Money mid = anchor;
  const int range = (2 * config.maxStepTicks) + 1;
  for (std::size_t i = 0; i < config.count; ++i) {
    const Money tick = instrument::hkEquityTick(mid);
    int step = static_cast<int>(rng.below(static_cast<std::uint64_t>(range))) - config.maxStepTicks;
    if (config.reversionTicks > 0 && tick > 0) {
      const Money distanceTicks = (mid - anchor) / tick;
      if (distanceTicks > config.reversionTicks) {
        step -= 1;
      } else if (distanceTicks < -config.reversionTicks) {
        step += 1;
      }
    }
    mid = std::clamp(snapDown(mid + (step * tick)), kMinMid, kMaxMid);
    const Money spreadTick = instrument::hkEquityTick(mid);

    QuoteEvent quote;
    quote.tsNs = config.startTsNs + (static_cast<std::int64_t>(i) * config.stepNs);
    quote.symbol = config.symbol;
    quote.bid = mid;
    quote.ask = mid + spreadTick;
    quote.last = (rng.next() & 1U) != 0 ? quote.ask : quote.bid;
    quote.bidSize = config.size;
    quote.askSize = config.size;
    out.push_back(std::move(quote));
  }
  return out;
}

}  // namespace futu_trader::backtest
