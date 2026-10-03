#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "futu_trader/market/quote.hpp"

namespace futu_trader::backtest {

/**
 * xorshift64*: a tiny PRNG whose output is defined by this code alone. `std::mt19937` is fixed by
 * the standard but `std::*_distribution` is not, so anything that must be identical across
 * compilers and platforms (golden backtests) uses this and integer arithmetic only.
 */
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_((seed * 0x9E3779B97F4A7C15ULL) + 0x1234567ULL) {
    if (state_ == 0) {
      state_ = 0x9E3779B97F4A7C15ULL;
    }
  }
  std::uint64_t next() {
    state_ ^= state_ >> 12U;
    state_ ^= state_ << 25U;
    state_ ^= state_ >> 27U;
    return state_ * 0x2545F4914F6CDD1DULL;
  }
  /** Uniform-ish integer in [0, n). n must be > 0. */
  std::uint64_t below(std::uint64_t n) { return (next() >> 11U) % n; }

 private:
  std::uint64_t state_;
};

struct SyntheticConfig {
  std::string symbol{"00700"};
  std::uint64_t seed{42};
  std::size_t count{20'000};
  std::int64_t startTsNs{1'767'600'000'000'000'000LL};  // arbitrary fixed instant (Jan 2026)
  std::int64_t stepNs{1'000'000'000};                   // one quote per second
  Money startMid{350'000};                              // 350.000 HKD
  int maxStepTicks{2};                                  // random move per quote, in ticks
  int reversionTicks{40};  // start pulling back once this many ticks from the anchor (0 = none)
  std::int64_t size{2000};
};

/**
 * Deterministic random-walk quotes on the HKEX tick grid with a one-tick spread. Same config ->
 * byte-identical output on every platform. A weak pull towards the start price keeps the series
 * bounded and gives mean-reversion strategies something (small) to find.
 */
std::vector<QuoteEvent> generateSyntheticQuotes(const SyntheticConfig& config);

}  // namespace futu_trader::backtest
