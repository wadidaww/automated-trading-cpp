#include "futu_trader/app/config.hpp"

#include <sys/stat.h>
#include <yaml-cpp/yaml.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>

#include "futu_trader/opend/types.hpp"

namespace futu_trader::app {
namespace {

struct ConfigError {
  std::string message;
};

[[noreturn]] void fail(const std::string& message) { throw ConfigError{message}; }

// Walks one mapping: tracks which keys were consumed so leftovers are reported as unknown.
class Section {
 public:
  Section(const YAML::Node& node, std::string path) : node_(node), path_(std::move(path)) {
    if (!node_.IsMap()) {
      fail(path_ + " must be a mapping");
    }
  }
  ~Section() noexcept(false) {
    if (std::uncaught_exceptions() == 0) {
      for (const auto& kv : node_) {
        const auto key = kv.first.as<std::string>();
        if (!used_.contains(key)) {
          fail("unknown key '" + qualified(key) + "'");
        }
      }
    }
  }

  bool has(const std::string& key) const { return static_cast<bool>(node_[key]); }

  YAML::Node require(const std::string& key) {
    used_.insert(key);
    const auto child = node_[key];
    if (!child || child.IsNull()) {
      fail("missing required key '" + qualified(key) + "'");
    }
    return child;
  }
  std::optional<YAML::Node> optional(const std::string& key) {
    used_.insert(key);
    auto child = node_[key];
    if (child && !child.IsNull()) {
      return child;
    }
    return std::nullopt;
  }
  Section sub(const std::string& key) { return {require(key), qualified(key)}; }
  std::string qualified(const std::string& key) const {
    return path_.empty() ? key : path_ + "." + key;
  }

  template <typename T>
  T get(const std::string& key) {
    const auto child = require(key);
    return convert<T>(child, key);
  }
  template <typename T>
  T getOr(const std::string& key, T fallback) {
    const auto child = optional(key);
    return child ? convert<T>(*child, key) : fallback;
  }

 private:
  template <typename T>
  T convert(const YAML::Node& child, const std::string& key) {
    if (!child.IsScalar()) {
      fail("'" + qualified(key) + "' must be a scalar");
    }
    try {
      return child.as<T>();
    } catch (const YAML::Exception&) {
      fail("'" + qualified(key) + "' has the wrong type");
    }
  }
  YAML::Node node_;
  std::string path_;
  std::set<std::string> used_;
};

std::int64_t positive(Section& s, const std::string& key) {
  const auto value = s.get<std::int64_t>(key);
  if (value <= 0) {
    fail("'" + s.qualified(key) + "' must be > 0");
  }
  return value;
}

// Whole HKD in the file -> mills, refusing overflow.
Money hkdToMills(Section& s, const std::string& key) {
  const auto hkd = positive(s, key);
  if (hkd > INT64_MAX / opend::kMoneyScale / 4) {
    fail("'" + s.qualified(key) + "' is implausibly large");
  }
  return hkd * opend::kMoneyScale;
}

std::uint16_t port(Section& s, const std::string& key, std::uint16_t fallback, bool required) {
  const auto value = required ? s.get<std::int64_t>(key) : s.getOr<std::int64_t>(key, fallback);
  if (value < 1 || value > 65535) {
    fail("'" + s.qualified(key) + "' must be 1..65535");
  }
  return static_cast<std::uint16_t>(value);
}

bool validHkCode(const std::string& code) {
  return !code.empty() && code.size() <= 5 &&
         std::all_of(code.begin(), code.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

AppConfig parseNode(const YAML::Node& root) {
  AppConfig cfg;
  Section top(root, "");

  const auto mode = top.get<std::string>("mode");
  if (mode == "simulate") {
    cfg.mode = Mode::kSimulate;
  } else if (mode == "real") {
    cfg.mode = Mode::kReal;
  } else {
    fail("'mode' must be 'simulate' or 'real'");
  }

  {
    auto s = top.sub("opend");
    cfg.opend.host = s.get<std::string>("host");
    cfg.opend.port = port(s, "port", 0, true);
    cfg.opend.requestTimeoutMs = s.getOr<std::int64_t>("request_timeout_ms", 5000);
    if (cfg.opend.requestTimeoutMs < 100 || cfg.opend.requestTimeoutMs > 60'000) {
      fail("'opend.request_timeout_ms' must be 100..60000");
    }
    cfg.opend.allowNonLoopback = s.getOr<bool>("allow_non_loopback", false);
  }
  {
    auto s = top.sub("account");
    const auto id = s.get<std::int64_t>("id");
    if (id <= 0) {
      fail("'account.id' must be > 0");
    }
    cfg.account.id = static_cast<std::uint64_t>(id);
    cfg.account.market = s.get<std::string>("market");
    if (cfg.account.market != "HK") {
      fail("'account.market' must be 'HK' (the only market supported)");
    }
  }
  if (const auto secrets = top.optional("secrets")) {
    Section s(*secrets, "secrets");
    cfg.tradePasswordMd5File = s.getOr<std::string>("trade_password_md5_file", "");
  }
  if (cfg.mode == Mode::kReal && cfg.tradePasswordMd5File.empty()) {
    fail("mode 'real' requires secrets.trade_password_md5_file");
  }

  {
    const auto list = top.require("symbols");
    if (!list.IsSequence() || list.size() == 0) {
      fail("'symbols' must be a non-empty list");
    }
    std::set<std::string> seen;
    for (const auto& item : list) {
      Section s(item, "symbols[]");
      SymbolConfig sym;
      sym.code = s.get<std::string>("code");
      if (!validHkCode(sym.code)) {
        fail("symbol code '" + sym.code +
             "' must be 1-5 digits, e.g. \"00700\" (quote it in YAML)");
      }
      sym.lotSize = positive(s, "lot");
      if (!seen.insert(sym.code).second) {
        fail("duplicate symbol " + sym.code);
      }
      cfg.symbols.push_back(std::move(sym));
    }
  }
  {
    auto s = top.sub("strategy");
    cfg.strategy.name = s.get<std::string>("name");
    if (cfg.strategy.name != "meanrev" && cfg.strategy.name != "maker" &&
        cfg.strategy.name != "buyhold") {
      fail("'strategy.name' must be meanrev, maker or buyhold");
    }
    cfg.strategy.symbol = s.get<std::string>("symbol");
    cfg.strategy.qty = positive(s, "qty");
    cfg.strategy.window = static_cast<std::size_t>(s.getOr<std::int64_t>("window", 60));
    cfg.strategy.entryZx10 = s.getOr<int>("entry_z_x10", 20);
    cfg.strategy.exitZx10 = s.getOr<int>("exit_z_x10", 0);
    if (cfg.strategy.entryZx10 < 1 || cfg.strategy.entryZx10 > 100 || cfg.strategy.exitZx10 < 0 ||
        cfg.strategy.exitZx10 > 100) {
      fail("'strategy.entry_z_x10' must be 1..100 and 'strategy.exit_z_x10' 0..100");
    }
    const auto cancelAfter = s.getOr<std::int64_t>("cancel_after_quotes", 8);
    if (cancelAfter < 1 || cancelAfter > 100'000) {
      fail("'strategy.cancel_after_quotes' must be 1..100000");
    }
    cfg.strategy.cancelAfterQuotes = static_cast<std::size_t>(cancelAfter);
    if (cfg.strategy.window < 2 || cfg.strategy.window > 100'000) {
      fail("'strategy.window' must be 2..100000");
    }
    const auto lot = std::find_if(cfg.symbols.begin(), cfg.symbols.end(),
                                  [&](const auto& sym) { return sym.code == cfg.strategy.symbol; });
    if (lot == cfg.symbols.end()) {
      fail("'strategy.symbol' is not listed under 'symbols'");
    }
    if (cfg.strategy.qty % lot->lotSize != 0) {
      fail("'strategy.qty' must be a multiple of the symbol's lot size");
    }
  }
  {
    auto s = top.sub("risk");
    cfg.risk.maxPositionNotional = hkdToMills(s, "max_position_notional_hkd");
    cfg.risk.maxPortfolioNotional = hkdToMills(s, "max_portfolio_notional_hkd");
    cfg.risk.maxDailyLoss = hkdToMills(s, "max_daily_loss_hkd");
    cfg.risk.maxOrderNotional = hkdToMills(s, "max_order_notional_hkd");
    const auto openOrders = positive(s, "max_open_orders");
    if (openOrders > 1000) {
      fail("'risk.max_open_orders' must be <= 1000");
    }
    cfg.risk.maxOpenOrders = static_cast<std::size_t>(openOrders);
    cfg.risk.concentrationLimit = s.get<double>("concentration_limit");
    // Written as a negated conjunction on purpose: it also refuses NaN, which De Morgan would not.
    if (!(cfg.risk.concentrationLimit > 0.0 &&  // NOLINT(readability-simplify-boolean-expr)
          cfg.risk.concentrationLimit <= 1.0)) {
      fail("'risk.concentration_limit' must be in (0, 1]");
    }
    cfg.risk.priceBandBps = positive(s, "price_band_bps");
    if (cfg.risk.priceBandBps > 5000) {
      fail("'risk.price_band_bps' must be <= 5000");
    }
    cfg.risk.maxQuoteAgeMs = positive(s, "max_quote_age_ms");
    if (cfg.risk.maxQuoteAgeMs > 60'000) {
      fail("'risk.max_quote_age_ms' must be <= 60000");
    }
    cfg.risk.allowShort = s.get<bool>("allow_short");  // explicit: shorting is never implied
    if (cfg.risk.maxPositionNotional > cfg.risk.maxPortfolioNotional) {
      fail("'risk.max_position_notional_hkd' exceeds 'risk.max_portfolio_notional_hkd'");
    }
    if (cfg.risk.maxOrderNotional > cfg.risk.maxPositionNotional) {
      fail("'risk.max_order_notional_hkd' exceeds 'risk.max_position_notional_hkd'");
    }
  }
  {
    auto s = top.sub("rate");
    const auto perWindow = positive(s, "max_per_window");
    if (perWindow > 1000) {
      fail("'rate.max_per_window' must be <= 1000 (OpenD allows about 15 per 30 s)");
    }
    cfg.rate.maxPerWindow = static_cast<std::size_t>(perWindow);
    cfg.rate.windowMs = positive(s, "window_ms");
    if (cfg.rate.windowMs > 3'600'000) {
      fail("'rate.window_ms' must be <= 3600000");
    }
    cfg.rate.reservedForCancels = static_cast<std::size_t>(positive(s, "reserved_for_cancels"));
    if (cfg.rate.reservedForCancels >= cfg.rate.maxPerWindow) {
      fail("'rate.reserved_for_cancels' must be below 'rate.max_per_window'");
    }
  }
  if (const auto node = top.optional("engine")) {
    Section s(*node, "engine");
    cfg.engine.ringCapacity =
        static_cast<std::size_t>(s.getOr<std::int64_t>("ring_capacity", 4096));
    if (cfg.engine.ringCapacity < 16 || cfg.engine.ringCapacity > (1U << 24)) {
      fail("'engine.ring_capacity' must be 16..16777216");
    }
    cfg.engine.maxQuoteAgeMs = s.getOr<std::int64_t>("max_quote_age_ms", 1000);
    if (cfg.engine.maxQuoteAgeMs < 1 || cfg.engine.maxQuoteAgeMs > 60'000) {
      fail("'engine.max_quote_age_ms' must be 1..60000 (0 would disable the stale-quote skip)");
    }
    cfg.engine.reconcileEverySec = s.getOr<std::int64_t>("reconcile_every_s", 30);
    if (cfg.engine.reconcileEverySec < 1 || cfg.engine.reconcileEverySec > 3600) {
      fail("'engine.reconcile_every_s' must be 1..3600 (reconciliation is not optional)");
    }
    if (s.has("cash_tolerance_hkd")) {
      cfg.engine.cashTolerance = hkdToMills(s, "cash_tolerance_hkd");
    }
    cfg.engine.busyPoll = s.getOr<bool>("busy_poll", false);
    cfg.engine.engineCpu = s.getOr<int>("engine_cpu", -1);
    cfg.engine.reconcilerCpu = s.getOr<int>("reconciler_cpu", -1);
    cfg.engine.realtimePriority = s.getOr<int>("realtime_priority", 0);
    if (cfg.engine.realtimePriority < 0 || cfg.engine.realtimePriority > 99) {
      fail("'engine.realtime_priority' must be 0..99");
    }
  }
  {
    auto s = top.sub("state");
    cfg.state.dir = s.get<std::string>("dir");
  }
  if (const auto node = top.optional("metrics")) {
    Section s(*node, "metrics");
    cfg.metrics.enabled = s.getOr<bool>("enabled", true);
    cfg.metrics.port = port(s, "port", 9464, false);
  }
  if (const auto node = top.optional("live")) {
    Section s(*node, "live");
    cfg.live.ackPhrase = s.getOr<std::string>("ack_phrase", "");
    cfg.live.promotionLog = s.getOr<std::string>("promotion_log", "");

    cfg.live.minSessionMinutes = s.getOr<std::int64_t>("min_session_minutes", 240);
    if (cfg.live.minSessionMinutes < 60 || cfg.live.minSessionMinutes > 1440) {
      fail("'live.min_session_minutes' must be 60..1440 (a shorter run is not a day's evidence)");
    }
    const auto cleanDays = s.getOr<std::int64_t>("required_clean_days", 5);
    if (cleanDays < 5 || cleanDays > 365) {
      fail("'live.required_clean_days' must be 5..365");
    }
    cfg.live.requiredCleanDays = static_cast<std::size_t>(cleanDays);
    if (!cfg.live.promotionLog.empty()) {
      const auto parent =
          std::filesystem::path(cfg.live.promotionLog).lexically_normal().parent_path();
      if (parent != std::filesystem::path(cfg.state.dir).lexically_normal()) {
        fail(
            "'live.promotion_log' must be a file directly inside 'state.dir' (a private "
            "directory)");
      }
    }
  }
  if (cfg.mode == Mode::kReal && !cfg.engine.cashTolerance) {
    fail("mode 'real' requires engine.cash_tolerance_hkd (cash reconciliation is mandatory)");
  }
  if (cfg.mode == Mode::kReal && (cfg.live.ackPhrase.empty() || cfg.live.promotionLog.empty())) {
    fail("mode 'real' requires live.ack_phrase and live.promotion_log");
  }
  return cfg;
}

}  // namespace

Result<AppConfig> parseConfig(const std::string& yamlText) {
  try {
    const YAML::Node root = YAML::Load(yamlText);
    if (!root.IsMap()) {
      return Error{ErrorCode::kInvalidArg, "config: top level must be a mapping"};
    }
    return parseNode(root);
  } catch (const ConfigError& e) {
    return Error{ErrorCode::kInvalidArg, "config: " + e.message};
  } catch (const YAML::Exception& e) {
    return Error{ErrorCode::kInvalidArg, std::string("config: invalid YAML: ") + e.what()};
  }
}

Result<AppConfig> loadConfigFile(const std::string& path) {
  // The config holds the risk limits: refuse one that other users could have edited.
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return Error{ErrorCode::kInvalidArg, "config: cannot read " + path};
  }
  if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return Error{ErrorCode::kInvalidArg,
                 "config: " + path + " is writable by group or others; chmod go-w it"};
  }
  if (info.st_size > (1 << 20)) {
    return Error{ErrorCode::kInvalidArg, "config: file too large"};
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return Error{ErrorCode::kInvalidArg, "config: cannot read " + path};
  }
  std::ostringstream text;
  text << in.rdbuf();
  return parseConfig(text.str());
}

}  // namespace futu_trader::app
