#include "futu_trader/app/application.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <regex>
#include <set>
#include <thread>

#include "../../tools/mock_opend/mock_opend.hpp"
#include "futu_trader/app/probe.hpp"
#include "futu_trader/execution/kill_switch.hpp"
#include "futu_trader/infra/wal.hpp"
#include "futu_trader/oms/journal_codec.hpp"
#include "test_support.hpp"

using namespace futu_trader;
using testing_support::waitFor;
using namespace futu_trader::app;

namespace {

std::string httpGet(std::uint16_t port, const std::string& path) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return {};
  }
  const std::string request = "GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
  (void)::write(fd, request.data(), request.size());
  std::string response;
  char buf[4096];
  ssize_t n = 0;
  while ((n = ::read(fd, buf, sizeof(buf))) > 0) {
    response.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return response;
}

struct Env {
  Env()
      : dir(std::filesystem::temp_directory_path() /
            ("futu_app_" + std::to_string(::getpid()) + "_" + std::to_string(++counter))) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    port = server.start();
  }
  ~Env() {
    server.stop();
    std::filesystem::remove_all(dir);
  }
  std::string yaml(const std::string& mode = "simulate", std::uint64_t account = 111,
                   const std::string& strategy = "buyhold") const {
    return "mode: " + mode + "\nopend: { host: \"127.0.0.1\", port: " + std::to_string(port) +
           ", request_timeout_ms: 400 }\naccount: { id: " + std::to_string(account) +
           ", market: HK }\n" +
           (mode == "real"
                ? "secrets: { trade_password_md5_file: " + (dir / "pwd").string() + " }\n"
                : "") +
           "symbols:\n  - { code: \"00700\", lot: 100 }\nstrategy: { name: " + strategy +
           ", symbol: \"00700\", qty: 100 }\n"
           "risk:\n  max_position_notional_hkd: 100000\n  max_portfolio_notional_hkd: 200000\n"
           "  max_order_notional_hkd: 50000\n  max_daily_loss_hkd: 5000\n  max_open_orders: 5\n"
           "  concentration_limit: 1.0\n  price_band_bps: 500\n  max_quote_age_ms: 5000\n"
           "  allow_short: false\nrate: { max_per_window: 15, window_ms: 30000, "
           "reserved_for_cancels: 5 }\n" +
           (mode == "real" ? "engine: { reconcile_every_s: 30, cash_tolerance_hkd: 1000000 }\n"
                           : "engine: { reconcile_every_s: 30 }\n") +
           "state: { dir: \"" + (dir / "state").string() +
           "\" }\nmetrics: { enabled: true, port: 9464 }\n" +
           (mode == "real" ? "live: { ack_phrase: I-ACCEPT-REAL-MONEY-222, promotion_log: " +
                                 (dir / "state" / "promo").string() + " }\n"
                           : "");
  }
  AppConfig config(const std::string& text) const {
    auto parsed = parseConfig(text);
    EXPECT_TRUE(parsed.ok()) << (parsed.ok() ? "" : parsed.error().message);
    AppConfig cfg = parsed.value();
    cfg.metrics.port = 1;  // replaced below: tests use an ephemeral port
    return cfg;
  }

  static inline int counter = 0;
  std::filesystem::path dir;
  mock::MockOpenD server;
  std::uint16_t port{0};
};

// Runs the trader on a thread until stopped.
struct Running {
  Running(const AppConfig& cfg, RunOptions options) : opts(std::move(options)) {
    opts.stop = &stop;
    opts.hardenProcess = false;  // would change this test process's core-dump limits
    opts.pollIntervalMs = 5;
    opts.log = [this](const std::string& line) {
      std::scoped_lock lock(logMu);
      lines.push_back(line);
    };
    opts.onRunning = [this](const RunningInfo& info) { ready.set_value(info); };
    config = cfg;
    config.metrics.port = 0;  // ephemeral
    // MetricsServerConfig treats 0 as ephemeral; the config validator only forbids it in files.
    worker = std::thread([this] { exitCode.set_value(runTrader(config, opts)); });
  }
  ~Running() {
    stop = true;
    if (worker.joinable()) {
      worker.join();
    }
  }
  int finish() {
    stop = true;
    auto result = exitCode.get_future().get();
    worker.join();
    return result;
  }
  std::string log() {
    std::scoped_lock lock(logMu);
    std::string all;
    for (const auto& l : lines) {
      all += l + "\n";
    }
    return all;
  }
  RunOptions opts;
  AppConfig config;
  std::atomic<bool> stop{false};
  std::promise<RunningInfo> ready;
  std::promise<int> exitCode;
  std::thread worker;
  std::mutex logMu;
  std::vector<std::string> lines;
};

int runToCompletion(const AppConfig& cfg, RunOptions options, std::string* logOut = nullptr) {
  options.hardenProcess = false;
  std::atomic<bool> stop{true};  // if startup somehow succeeds, stop immediately
  options.stop = &stop;
  options.pollIntervalMs = 5;
  std::string captured;
  options.log = [&](const std::string& line) { captured += line + "\n"; };
  AppConfig copy = cfg;
  copy.metrics.port = 0;
  const int code = runTrader(copy, options);
  if (logOut != nullptr) {
    *logOut = captured;
  }
  return code;
}

}  // namespace

TEST(Application, PaperTradingEndToEndAgainstTheMock) {
  Env env;
  Running run(env.config(env.yaml()), {});
  const auto info = run.ready.get_future().get();
  ASSERT_NE(info.metricsPort, 0);

  // Quotes arrive as the real wire pushes: a basic quote (last price) and the order book.
  ASSERT_TRUE(waitFor([&] {
    env.server.pushBasicQot("00700", 350.1);
    env.server.pushOrderBook("00700", 350.0, 4000, 350.2, 2000);
    return env.server.placeRequests() >= 1;
  })) << run.log();

  const auto orders = env.server.orders();
  ASSERT_EQ(orders.size(), 1U);
  EXPECT_EQ(orders[0].trdEnv, 0);  // SIMULATE, stamped by the venue
  EXPECT_EQ(orders[0].accId, 111U);
  EXPECT_EQ(orders[0].code, "00700");
  EXPECT_EQ(orders[0].remark.rfind("FT-", 0), 0U);  // our ClOrdId travels in the remark

  ASSERT_TRUE(env.server.fillOrderByRemark(orders[0].remark, 100, 350.2));
  const auto readyz = httpGet(info.metricsPort, "/readyz");
  EXPECT_NE(readyz.find("200"), std::string::npos) << readyz;
  // The place request reaching the broker is not yet the OMS recording the reply: wait for it.
  std::string metrics;
  ASSERT_TRUE(waitFor([&] {
    metrics = httpGet(info.metricsPort, "/metrics");
    return metrics.find("futu_oms_submits_total{status=\"accepted\"} 1") != std::string::npos;
  })) << metrics;
  EXPECT_NE(metrics.find("futu_opend_connected 1"), std::string::npos);
  EXPECT_NE(metrics.find("futu_wal_failed 0"), std::string::npos);

  EXPECT_EQ(run.finish(), kExitOk) << run.log();

  // The intent was written ahead of the order and is on disk.
  const auto wal = infra::readWal((env.dir / "state" / "orders.wal").string());
  ASSERT_EQ(wal.status, infra::WalStatus::kOk);
  std::size_t submits = 0;
  for (const auto& record : wal.records) {
    if (const auto s = oms::decodeSubmit(record)) {
      ++submits;
      EXPECT_EQ(s->clOrdId, orders[0].remark);
    }
  }
  EXPECT_EQ(submits, 1U);
}

TEST(Application, StopCancelsRestingOrdersWithoutTrippingTheKillSwitch) {
  Env env;
  Running run(env.config(env.yaml()), {});
  run.ready.get_future().get();
  ASSERT_TRUE(waitFor([&] {
    env.server.pushBasicQot("00700", 350.1);
    env.server.pushOrderBook("00700", 350.0, 4000, 350.2, 2000);
    return env.server.placeRequests() >= 1;
  }));
  EXPECT_EQ(run.finish(), kExitOk);
  EXPECT_NE(run.log().find("cancel requested for 1 order"), std::string::npos) << run.log();
  EXPECT_FALSE(std::filesystem::exists(env.dir / "state" / "kill_switch.tripped"));
}

TEST(Application, RefusesSimulateConfigPointedAtARealAccountAndNeverTrades) {
  Env env;
  std::string log;
  EXPECT_EQ(runToCompletion(env.config(env.yaml("simulate", 222)), {}, &log), kExitRefused);
  EXPECT_NE(log.find("REAL account"), std::string::npos) << log;
  EXPECT_EQ(env.server.placeRequests(), 0U);
}

TEST(Application, RefusesAnUnknownAccount) {
  Env env;
  EXPECT_EQ(runToCompletion(env.config(env.yaml("simulate", 999)), {}), kExitRefused);
}

TEST(Application, LiveFlagWithSimulateConfigIsAUsageError) {
  Env env;
  RunOptions options;
  options.live = true;
  EXPECT_EQ(runToCompletion(env.config(env.yaml()), options), kExitUsage);
  EXPECT_EQ(env.server.connectionCount(), 0U);  // refused before touching OpenD
}

TEST(Application, RealModeIsRefusedWithoutEachGateAndNeverPlacesAnOrder) {
  Env env;
  {
    std::ofstream(env.dir / "pwd") << "0123456789abcdef0123456789abcdef\n";
    ::chmod((env.dir / "pwd").c_str(), 0600);
    std::filesystem::create_directories(env.dir / "state");
    std::ofstream(env.dir / "state" / "promo") << "2020-01-01 clean\n";  // stale and too short
    ::chmod((env.dir / "state" / "promo").c_str(), 0600);
  }
  const auto cfg = env.config(env.yaml("real", 222));
  {
    RunOptions options;  // no --live, no env var
    std::string log;
    EXPECT_EQ(runToCompletion(cfg, options, &log), kExitRefused) << log;
  }
  {
    RunOptions options;
    options.live = true;
    options.liveEnv = "I_UNDERSTAND_REAL_MONEY";
    std::string log;
    EXPECT_EQ(runToCompletion(cfg, options, &log), kExitRefused) << log;  // promotion record bad
    EXPECT_NE(log.find("promotion"), std::string::npos) << log;
  }
  EXPECT_EQ(env.server.placeRequests(), 0U);
}

TEST(Application, RealModeRefusesAWorldReadableSecret) {
  Env env;
  std::ofstream(env.dir / "pwd") << "0123456789abcdef0123456789abcdef\n";
  ::chmod((env.dir / "pwd").c_str(), 0644);
  RunOptions options;
  options.live = true;
  options.liveEnv = "I_UNDERSTAND_REAL_MONEY";
  std::string log;
  EXPECT_EQ(runToCompletion(env.config(env.yaml("real", 222)), options, &log), kExitRefused);
  EXPECT_EQ(env.server.unlockRequests(), 0U);  // never even sent the secret
}

TEST(Application, ATrippedKillSwitchBlocksStartupUntilAHumanResetsIt) {
  Env env;
  const auto cfg = env.config(env.yaml());
  std::filesystem::create_directories(env.dir / "state");
  {
    execution::KillSwitch kill((env.dir / "state" / "kill_switch.tripped").string());
    kill.trip("operator test");
  }
  std::string log;
  EXPECT_EQ(runToCompletion(cfg, {}, &log), kExitRefused);
  EXPECT_NE(log.find("kill switch is tripped"), std::string::npos) << log;
  EXPECT_EQ(env.server.connectionCount(), 0U);

  EXPECT_EQ(resetKillSwitch(cfg, "", nullptr), kExitUsage);  // an empty operator name is refused
  EXPECT_EQ(resetKillSwitch(cfg, "alice", [](const std::string&) {}), kExitOk);
  Running run(cfg, {});
  run.ready.get_future().get();
  EXPECT_EQ(run.finish(), kExitOk) << run.log();
}

TEST(Application, ACorruptWalRefusesStartup) {
  Env env;
  const auto cfg = env.config(env.yaml());
  std::filesystem::create_directories(env.dir / "state");
  std::ofstream(env.dir / "state" / "orders.wal") << "garbage that is not a log at all";
  std::string log;
  EXPECT_EQ(runToCompletion(cfg, {}, &log), kExitRefused);
  EXPECT_NE(log.find("corrupt"), std::string::npos) << log;
}

TEST(Application, UnreachableOpenDExitsWithConnectError) {
  Env env;
  const auto cfg = env.config(env.yaml());
  env.server.stop();
  EXPECT_EQ(runToCompletion(cfg, {}), kExitConnect);
}

TEST(Application, CrashBetweenSendAndReplyDoesNotDuplicateTheOrderAfterRestart) {
  Env env;
  const auto cfg = env.config(env.yaml());
  std::string firstRemark;
  {
    Running first(cfg, {});
    first.ready.get_future().get();
    mock::Faults faults;
    faults.dropResponses = 1;  // the broker accepts the order but its reply never arrives
    env.server.setFaults(faults);
    env.server.setSuppressPushes(true);  // ...and the order-update push is lost too
    ASSERT_TRUE(waitFor([&] {
      env.server.pushBasicQot("00700", 350.1);
      env.server.pushOrderBook("00700", 350.0, 4000, 350.2, 2000);
      return env.server.placeRequests() >= 1;
    })) << first.log();
    ASSERT_TRUE(waitFor([&] { return !env.server.orders().empty(); }));
    firstRemark = env.server.orders()[0].remark;
    // "Crash": stop without the graceful path mattering; the WAL and the broker are what remain.
    first.finish();
  }
  env.server.setSuppressPushes(false);
  ASSERT_EQ(env.server.orders().size(), 1U);

  Running second(cfg, {});
  const auto info = second.ready.get_future().get();
  EXPECT_NE(second.log().find("restored 1 order intent"), std::string::npos) << second.log();
  // Startup (bootstrap + first reconciliation) matched the restored intent to the broker's order
  // by its ClOrdId remark: it is ours again, not a stranger, and nothing is left unresolved.
  const auto metrics = httpGet(info.metricsPort, "/metrics");
  EXPECT_NE(metrics.find("futu_oms_restored_intents_total 1"), std::string::npos) << metrics;
  EXPECT_NE(metrics.find("futu_oms_unresolved_orders 0"), std::string::npos) << metrics;
  // The first run's graceful stop reconciled, learned the lost-reply order's broker id and
  // cancelled it, so the restored intent now matches a cancelled order, not a working one.
  EXPECT_NE(metrics.find("futu_oms_live_orders 0"), std::string::npos) << metrics;
  second.finish();
  std::size_t withFirstRemark = 0;
  for (const auto& o : env.server.orders()) {
    withFirstRemark += o.remark == firstRemark ? 1U : 0U;
  }
  EXPECT_EQ(withFirstRemark, 1U);  // never a second copy of the lost-reply order
}

// The alert rules and the dashboard are written by hand; this keeps them honest. A rule that names
// a metric the process does not export would never fire, silently.
TEST(Ops, AlertRulesAndDashboardOnlyReferenceMetricsTheProcessExports) {
  Env env;
  Running run(env.config(env.yaml()), {});
  const auto info = run.ready.get_future().get();
  const auto exposition = httpGet(info.metricsPort, "/metrics");
  ASSERT_NE(exposition.find("# TYPE futu_oms_live_orders gauge"), std::string::npos) << exposition;
  std::set<std::string> exported;
  const std::regex typeLine("# TYPE (futu_[a-z0-9_]+) ");
  for (std::sregex_iterator it(exposition.begin(), exposition.end(), typeLine), end; it != end;
       ++it) {
    exported.insert((*it)[1]);
  }
  ASSERT_GT(exported.size(), 30U);

  for (const char* file : {"/prometheus/alerts.yml", "/grafana/dashboards/futu_trader.json"}) {
    std::ifstream in(std::string(FUTU_OPS_DIR) + file);
    ASSERT_TRUE(in.good()) << file;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::regex ident("futu_[a-z0-9_]+");
    std::size_t checked = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), ident), end; it != end; ++it) {
      std::string name = it->str();
      for (const char* suffix : {"_bucket", "_sum", "_count"}) {
        const std::string s(suffix);
        if (name.size() > s.size() && name.compare(name.size() - s.size(), s.size(), s) == 0) {
          name.resize(name.size() - s.size());
          break;
        }
      }
      // Names that are not metrics: the job, rule groups, and a path mentioned in an annotation.
      if (name == "futu_trader" || name == "futu_trader_critical" ||
          name == "futu_trader_warning" || name == "futu_trader_meta" || name == "futu_proto") {
        continue;
      }
      EXPECT_TRUE(exported.contains(name)) << file << " references unknown metric " << name;
      ++checked;
    }
    EXPECT_GT(checked, 10U) << file;
  }
  run.finish();
}

TEST(Probe, ReadyWhileTradingAndNotReadyOnceStoppedOrWhenNothingListens) {
  Env env;
  std::uint16_t port = 0;
  {
    Running run(env.config(env.yaml()), {});
    port = run.ready.get_future().get().metricsPort;
    EXPECT_TRUE(probeReady(port, std::chrono::seconds(2)));
    env.server.stop();  // OpenD goes away: the link drops, the process halts, readiness must fall
    EXPECT_TRUE(waitFor([&] { return !probeReady(port, std::chrono::milliseconds(300)); }));
    EXPECT_EQ(run.finish(), kExitHalted) << run.log();
  }
  EXPECT_FALSE(probeReady(port, std::chrono::milliseconds(300)));  // process gone
}

// ---- Promotion evidence written by SIMULATE sessions
// ----------------------------------------------

namespace {
std::string readFile(const std::filesystem::path& p) {
  std::ifstream in(p);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
}  // namespace

TEST(Application, ACleanSimulateSessionRecordsACleanDayAndAHaltRecordsADirtyOne) {
  Env env;
  const auto promo = env.dir / "promotion.log";
  auto cfg = env.config(env.yaml());
  cfg.live.promotionLog = promo.string();
  cfg.live.minSessionMinutes = 0;
  std::string firstLog;
  {
    RunOptions options;
    options.today = "2026-10-01";
    Running run(cfg, options);
    run.ready.get_future().get();
    ASSERT_TRUE(waitFor([&] {  // a clean day needs evidence the strategy actually ran
      env.server.pushBasicQot("00700", 350.1);
      env.server.pushOrderBook("00700", 350.0, 4000, 350.2, 2000);
      return env.server.placeRequests() >= 1;
    }));
    EXPECT_EQ(run.finish(), kExitOk) << run.log();
    firstLog = run.log();
  }
  EXPECT_EQ(readFile(promo), "2026-10-01 clean\n") << firstLog;
  EXPECT_EQ(std::filesystem::status(promo).permissions() & std::filesystem::perms::others_read,
            std::filesystem::perms::none);

  {
    RunOptions options;
    options.today = "2026-10-02";
    Running run(cfg, options);
    run.ready.get_future().get();
    env.server.stop();  // OpenD vanishes: trading halts
    EXPECT_EQ(run.finish(), kExitHalted) << run.log();
  }
  EXPECT_EQ(readFile(promo), "2026-10-01 clean\n2026-10-02 dirty\n");
}

TEST(Application, AShortCleanSessionRecordsNothing) {
  Env env;
  const auto promo = env.dir / "promotion.log";
  auto cfg = env.config(env.yaml());
  cfg.live.promotionLog = promo.string();
  cfg.live.minSessionMinutes = 240;
  RunOptions options;
  options.today = "2026-10-01";
  Running run(cfg, options);
  run.ready.get_future().get();
  EXPECT_EQ(run.finish(), kExitOk);
  EXPECT_FALSE(std::filesystem::exists(promo));
  EXPECT_NE(run.log().find("nothing recorded"), std::string::npos) << run.log();
}

TEST(Application, AnIdleSessionEarnsNoCleanDay) {
  Env env;
  const auto promo = env.dir / "promotion.log";
  auto cfg = env.config(env.yaml());
  cfg.live.promotionLog = promo.string();
  cfg.live.minSessionMinutes = 0;
  RunOptions options;
  options.today = "2026-10-01";
  Running run(cfg, options);
  run.ready.get_future().get();
  EXPECT_EQ(run.finish(), kExitOk);
  EXPECT_FALSE(std::filesystem::exists(promo));
}

TEST(Application, WhenOpenDDiesWithAnOrderRestingTheExitSaysOrdersMayBeResting) {
  Env env;
  RunOptions options;
  options.shutdownDeadlineMs = 400;
  options.shutdownRetryMs = 50;
  Running run(env.config(env.yaml()), options);
  run.ready.get_future().get();
  ASSERT_TRUE(waitFor([&] {
    env.server.pushBasicQot("00700", 350.1);
    env.server.pushOrderBook("00700", 350.0, 4000, 350.2, 2000);
    return env.server.placeRequests() >= 1;
  }));
  ASSERT_EQ(env.server.orders().size(), 1U);
  env.server.stop();  // the link drops with the order resting: every cancel will fail
  EXPECT_EQ(run.finish(), kExitOrdersMayRest) << run.log();
  EXPECT_NE(run.log().find("orders may still be resting"), std::string::npos) << run.log();
  EXPECT_NE(run.log().find("failed"), std::string::npos);  // the failed cancels are in the log
}

TEST(Application, AHaltFileInTheStateDirRefusesStartup) {
  Env env;
  std::filesystem::create_directories(env.dir / "state");
  { std::ofstream(env.dir / "state" / "HALT") << "stop"; }
  std::string log;
  EXPECT_EQ(runToCompletion(env.config(env.yaml()), {}, &log), kExitRefused);
  EXPECT_NE(log.find("HALT"), std::string::npos) << log;
  EXPECT_EQ(env.server.connectionCount(), 0U);
}

TEST(Application, RealModeRunsTheWholeGateTradesAndRelocksOnExit) {
  Env env;
  std::filesystem::create_directories(env.dir / "state");
  ::chmod((env.dir / "state").c_str(), 0700);
  {
    std::ofstream(env.dir / "pwd") << "0123456789abcdef0123456789abcdef\n";
    ::chmod((env.dir / "pwd").c_str(), 0600);
    std::ofstream(env.dir / "state" / "promo") << "2026-09-26 clean\n2026-09-27 clean\n2026-09-28 "
                                                  "clean\n2026-09-29 clean\n2026-09-30 clean\n";
    ::chmod((env.dir / "state" / "promo").c_str(), 0600);
  }
  RunOptions options;
  options.live = true;
  options.liveEnv = "I_UNDERSTAND_REAL_MONEY";
  options.today = "2026-10-01";
  Running run(env.config(env.yaml("real", 222)), options);
  run.ready.get_future().get();
  ASSERT_TRUE(waitFor([&] {
    env.server.pushBasicQot("00700", 350.1);
    env.server.pushOrderBook("00700", 350.0, 4000, 350.2, 2000);
    return env.server.placeRequests() >= 1;
  })) << run.log();
  ASSERT_EQ(env.server.orders().size(), 1U);
  EXPECT_EQ(env.server.orders()[0].trdEnv, 1);  // REAL, stamped by the venue, only after the gate
  EXPECT_EQ(env.server.orders()[0].accId, 222U);
  EXPECT_EQ(env.server.unlockRequests(), 1U);
  EXPECT_EQ(run.finish(), kExitOk) << run.log();
  EXPECT_EQ(env.server.unlockRequests(), 2U);  // unlock + the relock on the way out
}

TEST(Application, EveryRealRefusalHappensBeforeTheAccountIsUnlocked) {
  Env env;
  std::filesystem::create_directories(env.dir / "state");
  {
    std::ofstream(env.dir / "pwd") << "0123456789abcdef0123456789abcdef\n";
    ::chmod((env.dir / "pwd").c_str(), 0600);
    std::ofstream(env.dir / "state" / "promo") << "2020-01-01 clean\n";
    ::chmod((env.dir / "state" / "promo").c_str(), 0600);
  }
  const auto cfg = env.config(env.yaml("real", 222));
  EXPECT_EQ(runToCompletion(cfg, {}), kExitRefused);  // no --live, no env var
  RunOptions stale;
  stale.live = true;
  stale.liveEnv = "I_UNDERSTAND_REAL_MONEY";
  stale.today = "2026-10-01";
  EXPECT_EQ(runToCompletion(cfg, stale), kExitRefused);  // promotion record stale
  RunOptions wrongEnv;
  wrongEnv.live = true;
  wrongEnv.liveEnv = "yes";
  EXPECT_EQ(runToCompletion(cfg, wrongEnv), kExitRefused);
  EXPECT_EQ(env.server.unlockRequests(), 0U);  // the password hash never left the process
  EXPECT_EQ(env.server.placeRequests(), 0U);
}
