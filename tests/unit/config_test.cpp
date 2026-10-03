#include "futu_trader/app/config.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>

using namespace futu_trader;
using namespace futu_trader::app;

namespace {

const std::string kValid = R"(
mode: simulate
opend: { host: "127.0.0.1", port: 11111 }
account: { id: 111, market: HK }
symbols:
  - { code: "00700", lot: 100 }
  - { code: "09988", lot: 100 }
strategy: { name: meanrev, symbol: "00700", qty: 200 }
risk:
  max_position_notional_hkd: 50000
  max_portfolio_notional_hkd: 100000
  max_order_notional_hkd: 40000
  max_daily_loss_hkd: 2000
  max_open_orders: 5
  concentration_limit: 0.5
  price_band_bps: 200
  max_quote_age_ms: 2000
  allow_short: false
rate: { max_per_window: 15, window_ms: 30000, reserved_for_cancels: 5 }
state: { dir: "var/x" }
)";

std::string replace(std::string text, const std::string& from, const std::string& to) {
  const auto at = text.find(from);
  EXPECT_NE(at, std::string::npos) << "test setup: '" << from << "' not found";
  if (at != std::string::npos) {
    text.replace(at, from.size(), to);
  }
  return text;
}

void expectRefused(const std::string& yaml, const std::string& mentions) {
  const auto cfg = parseConfig(yaml);
  ASSERT_FALSE(cfg.ok()) << "accepted a config that should be refused (" << mentions << ")";
  EXPECT_NE(cfg.error().message.find(mentions), std::string::npos) << cfg.error().message;
}

}  // namespace

TEST(Config, ParsesAValidConfigAndConvertsMoneyToMills) {
  const auto cfg = parseConfig(kValid);
  ASSERT_TRUE(cfg.ok()) << cfg.error().message;
  const auto& c = cfg.value();
  EXPECT_EQ(c.mode, Mode::kSimulate);
  EXPECT_EQ(c.opend.port, 11111);
  EXPECT_EQ(c.account.id, 111U);
  ASSERT_EQ(c.symbols.size(), 2U);
  EXPECT_EQ(c.symbols[0].code, "00700");              // quoted: leading zeros preserved
  EXPECT_EQ(c.risk.maxPositionNotional, 50'000'000);  // HKD 50,000 = 50,000,000 mills
  EXPECT_EQ(c.risk.maxDailyLoss, 2'000'000);
  EXPECT_EQ(c.rate.reservedForCancels, 5U);
  EXPECT_TRUE(c.metrics.enabled);  // optional sections take their defaults
  EXPECT_EQ(c.engine.reconcileEverySec, 30);
}

TEST(Config, EveryRiskLimitIsMandatory) {
  for (const char* key :
       {"max_position_notional_hkd", "max_portfolio_notional_hkd", "max_order_notional_hkd",
        "max_daily_loss_hkd", "max_open_orders", "concentration_limit", "price_band_bps",
        "max_quote_age_ms", "allow_short"}) {
    const std::regex line(std::string("\\n  ") + key + ":[^\\n]*");
    expectRefused(std::regex_replace(kValid, line, ""), key);
  }
}

TEST(Config, ZeroOrNegativeLimitsAreRefusedNotTreatedAsUnlimited) {
  expectRefused(replace(kValid, "max_daily_loss_hkd: 2000", "max_daily_loss_hkd: 0"),
                "max_daily_loss_hkd");
  expectRefused(replace(kValid, "max_open_orders: 5", "max_open_orders: -1"), "max_open_orders");
  expectRefused(replace(kValid, "concentration_limit: 0.5", "concentration_limit: 0"),
                "concentration_limit");
  expectRefused(replace(kValid, "concentration_limit: 0.5", "concentration_limit: 1.5"),
                "concentration_limit");
}

TEST(Config, InconsistentLimitsAreRefused) {
  expectRefused(
      replace(kValid, "max_position_notional_hkd: 50000", "max_position_notional_hkd: 200000"),
      "exceeds");
  expectRefused(replace(kValid, "max_order_notional_hkd: 40000", "max_order_notional_hkd: 60000"),
                "exceeds");
  expectRefused(replace(kValid, "reserved_for_cancels: 5", "reserved_for_cancels: 15"),
                "reserved_for_cancels");
}

TEST(Config, UnknownKeysAreRefusedSoATypoCannotFallBackToADefault) {
  expectRefused(
      replace(kValid, "allow_short: false", "allow_short: false\n  max_dailly_loss_hkd: 2000"),
      "risk.max_dailly_loss_hkd");
  expectRefused(kValid + "surprise: 1\n", "surprise");
  expectRefused(replace(kValid, "port: 11111 }", "port: 11111, hots: x }"), "opend.hots");
}

TEST(Config, WrongTypesAndMissingSectionsAreNamed) {
  expectRefused(replace(kValid, "max_open_orders: 5", "max_open_orders: lots"), "max_open_orders");
  expectRefused(replace(kValid, "allow_short: false", "allow_short: maybe"), "allow_short");
  expectRefused(
      replace(kValid, "rate: { max_per_window: 15, window_ms: 30000, reserved_for_cancels: 5 }",
              ""),
      "rate");
  expectRefused("- just\n- a list\n", "mapping");
  expectRefused("mode: [unclosed", "invalid YAML");
  expectRefused("", "mapping");
}

TEST(Config, ModeMustBeExplicitAndSimulateOrReal) {
  expectRefused(replace(kValid, "mode: simulate", "mode: paper"), "mode");
  expectRefused(replace(kValid, "mode: simulate\n", ""), "mode");
}

TEST(Config, RealModeNeedsItsSecretCashToleranceAndTheLiveSection) {
  const std::string real = replace(kValid, "mode: simulate", "mode: real");
  expectRefused(real, "trade_password_md5_file");
  const std::string withSecret =
      replace(real, "symbols:", "secrets: { trade_password_md5_file: /tmp/x }\nsymbols:");
  expectRefused(withSecret, "cash_tolerance_hkd");
  const std::string withCash =
      replace(withSecret, "state: { dir: \"var/x\" }",
              "engine: { cash_tolerance_hkd: 100 }\nstate: { dir: \"var/x\" }");
  expectRefused(withCash, "live.ack_phrase");
  const auto ok = parseConfig(withCash + "live: { ack_phrase: a, promotion_log: var/x/promo }\n");
  ASSERT_TRUE(ok.ok()) << ok.error().message;
  EXPECT_EQ(ok.value().mode, Mode::kReal);
  EXPECT_EQ(*ok.value().engine.cashTolerance, 100'000);
  expectRefused(
      withCash + "live: { ack_phrase: a, promotion_log: var/x/promo, required_clean_days: 1 }\n",
      "required_clean_days");
  expectRefused(withCash + "live: { ack_phrase: a, promotion_log: /tmp/elsewhere/promo }\n",
                "inside 'state.dir'");
  expectRefused(
      withCash + "live: { ack_phrase: a, promotion_log: var/x/promo, min_session_minutes: 5 }\n",
      "min_session_minutes");
}

TEST(Config, SymbolsAreValidatedAndTheStrategyMustTradeOneOfThem) {
  expectRefused(replace(kValid, "code: \"00700\"", "code: \"700.HK\""), "700.HK");
  expectRefused(replace(kValid, "{ code: \"00700\", lot: 100 }", "{ code: \"00700\" }"), "lot");
  expectRefused(replace(kValid, "{ code: \"09988\", lot: 100 }", "{ code: \"00700\", lot: 100 }"),
                "duplicate");
  expectRefused(replace(kValid, "symbol: \"00700\", qty", "symbol: \"01810\", qty"),
                "strategy.symbol");
  expectRefused(replace(kValid, "qty: 200", "qty: 150"), "lot size");
  expectRefused(replace(kValid, "name: meanrev", "name: yolo"), "strategy.name");
}

TEST(Config, PortAccountAndMarketAreChecked) {
  expectRefused(replace(kValid, "port: 11111", "port: 0"), "port");
  expectRefused(replace(kValid, "port: 11111", "port: 70000"), "port");
  expectRefused(replace(kValid, "id: 111", "id: 0"), "account.id");
  expectRefused(replace(kValid, "market: HK", "market: US"), "account.market");
}

TEST(Config, HugeMoneyDoesNotOverflow) {
  expectRefused(replace(kValid, "max_position_notional_hkd: 50000",
                        "max_position_notional_hkd: 9000000000000000"),
                "implausibly large");
}

TEST(Config, ShippedExampleConfigsParse) {
  const auto paper = loadConfigFile(std::string(FUTU_CONFIG_DIR) + "/paper.yaml");
  ASSERT_TRUE(paper.ok()) << paper.error().message;
  EXPECT_EQ(paper.value().mode, Mode::kSimulate);
  const auto live = loadConfigFile(std::string(FUTU_CONFIG_DIR) + "/live.example.yaml");
  ASSERT_TRUE(live.ok()) << live.error().message;
  EXPECT_EQ(live.value().mode, Mode::kReal);
  EXPECT_FALSE(loadConfigFile("/nonexistent/x.yaml").ok());
}

TEST(Config, HostileNumbersAreBoundedBeforeTheyCanOverflowOrDisableACheck) {
  expectRefused(replace(kValid, "window_ms: 30000", "window_ms: 9000000000000000"), "window_ms");
  expectRefused(replace(kValid, "max_quote_age_ms: 2000", "max_quote_age_ms: 9000000000000"),
                "max_quote_age_ms");
  expectRefused(replace(kValid, "max_per_window: 15", "max_per_window: 100000"), "max_per_window");
  expectRefused(replace(kValid, "state: { dir: \"var/x\" }",
                        "engine: { max_quote_age_ms: 0 }\nstate: { dir: \"var/x\" }"),
                "engine.max_quote_age_ms");
  expectRefused(replace(kValid, "state: { dir: \"var/x\" }",
                        "engine: { reconcile_every_s: 9000000000000 }\nstate: { dir: \"var/x\" }"),
                "reconcile_every_s");
  expectRefused(replace(kValid, "qty: 200 }", "qty: 200, cancel_after_quotes: -5 }"),
                "cancel_after_quotes");
  expectRefused(replace(kValid, "qty: 200 }", "qty: 200, entry_z_x10: 0 }"), "entry_z_x10");
}

TEST(Config, ARiskConfigThatOthersCanWriteIsRefused) {
  const auto path = std::filesystem::temp_directory_path() / "futu_cfg_perm.yaml";
  { std::ofstream(path) << kValid; }
  ::chmod(path.c_str(), 0600);
  EXPECT_TRUE(loadConfigFile(path.string()).ok());
  ::chmod(path.c_str(), 0666);
  const auto loose = loadConfigFile(path.string());
  ASSERT_FALSE(loose.ok());
  EXPECT_NE(loose.error().message.find("writable"), std::string::npos);
  std::filesystem::remove(path);
}
