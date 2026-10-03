#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/core/types.hpp"

namespace futu_trader::app {

enum class Mode : std::uint8_t { kSimulate, kReal };

struct SymbolConfig {
  std::string code;         // HK code, e.g. "00700"
  std::int64_t lotSize{0};  // board lot: it differs per stock, so it is never defaulted
};

/** Everything the trader needs, validated. Money is int64 mills (HKD x 1000). */
struct AppConfig {
  Mode mode{Mode::kSimulate};

  struct OpenD {
    std::string host;
    std::uint16_t port{0};
    std::int64_t requestTimeoutMs{5000};
    bool allowNonLoopback{false};
  } opend;

  struct Account {
    std::uint64_t id{0};
    std::string market{"HK"};
  } account;

  /** File holding the MD5 of the trade password. Required for REAL, ignored for SIMULATE. */
  std::string tradePasswordMd5File;

  std::vector<SymbolConfig> symbols;

  struct Strategy {
    std::string name;    // meanrev | maker | buyhold
    std::string symbol;  // must be one of `symbols`
    std::int64_t qty{0};
    std::size_t window{60};
    int entryZx10{20};
    int exitZx10{0};
    std::size_t cancelAfterQuotes{8};
  } strategy;

  /** Every limit is mandatory: an absent limit is a refused config, never "unlimited". */
  struct Risk {
    Money maxPositionNotional{0};
    Money maxPortfolioNotional{0};
    Money maxDailyLoss{0};
    Money maxOrderNotional{0};
    std::size_t maxOpenOrders{0};
    double concentrationLimit{0.0};
    std::int64_t priceBandBps{0};
    std::int64_t maxQuoteAgeMs{0};
    bool allowShort{false};
  } risk;

  struct Rate {
    std::size_t maxPerWindow{0};
    std::int64_t windowMs{0};
    std::size_t reservedForCancels{0};
  } rate;

  struct Engine {
    std::size_t ringCapacity{4096};
    std::int64_t maxQuoteAgeMs{1000};
    std::int64_t reconcileEverySec{30};
    bool busyPoll{false};
    int engineCpu{-1};
    int reconcilerCpu{-1};
    int realtimePriority{0};
  } engine;

  struct State {
    std::string dir;  // WAL, persistent kill switch and kill-flag file live here
  } state;

  struct Metrics {
    bool enabled{true};
    std::uint16_t port{9464};
  } metrics;

  struct Live {
    std::string ackPhrase;
    std::string promotionLog;
    std::size_t requiredCleanDays{5};
    /**
     * SIMULATE only: a session that stops cleanly after at least this long is recorded as a clean
     * day in `promotionLog` (shorter clean sessions record nothing; any halt or reconciliation
     * problem records a dirty day regardless of length).
     */
    std::int64_t minSessionMinutes{240};
  } live;
};

/**
 * Parses and validates. Strict by design: unknown keys, wrong types, missing required keys and
 * out-of-range values are all errors naming the offending key, because a typo in a risk limit must
 * not silently fall back to a default. Money in the file is whole HKD (`*_hkd`).
 */
Result<AppConfig> parseConfig(const std::string& yamlText);
Result<AppConfig> loadConfigFile(const std::string& path);

}  // namespace futu_trader::app
