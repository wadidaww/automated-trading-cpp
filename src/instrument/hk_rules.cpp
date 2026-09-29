#include "futu_trader/instrument/hk_rules.hpp"

#include <array>

namespace futu_trader::instrument {

namespace {

struct Band {
  Money fromMills;  // inclusive
  Money tickMills;
};

// Upper end of the table: 9,995.000 HKD is the last valid price.
constexpr Money kMinPrice = 10;         // 0.010
constexpr Money kMaxPrice = 9'995'000;  // 9995.000

constexpr std::array<Band, 11> kBands{{
    {10, 1},            // 0.01   - 0.25   : 0.001
    {250, 5},           // 0.25   - 0.50   : 0.005
    {500, 10},          // 0.50   - 10.00  : 0.010
    {10'000, 20},       // 10.00  - 20.00  : 0.020
    {20'000, 50},       // 20.00  - 100.00 : 0.050
    {100'000, 100},     // 100.00 - 200.00 : 0.100
    {200'000, 200},     // 200.00 - 500.00 : 0.200
    {500'000, 500},     // 500.00 - 1000.00: 0.500
    {1'000'000, 1000},  // 1000   - 2000   : 1.000
    {2'000'000, 2000},  // 2000   - 5000   : 2.000
    {5'000'000, 5000},  // 5000   - 9995   : 5.000
}};

}  // namespace

Money hkEquityTick(Money priceMills) {
  if (priceMills < kMinPrice || priceMills > kMaxPrice) {
    return 0;
  }
  Money tick = 0;
  for (const Band& band : kBands) {
    if (priceMills >= band.fromMills) {
      tick = band.tickMills;
    }
  }
  return tick;
}

bool isTickAligned(Money priceMills) {
  const Money tick = hkEquityTick(priceMills);
  return tick > 0 && priceMills % tick == 0;
}

void InstrumentTable::add(InstrumentInfo info) {
  const std::string code = info.code;
  byCode_[code] = std::move(info);
}

std::optional<InstrumentInfo> InstrumentTable::find(const std::string& code) const {
  const auto found = byCode_.find(code);
  if (found == byCode_.end()) {
    return std::nullopt;
  }
  return found->second;
}

}  // namespace futu_trader::instrument
