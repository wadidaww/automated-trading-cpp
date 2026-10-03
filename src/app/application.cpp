#include "futu_trader/app/application.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include "futu_trader/app/metrics_bindings.hpp"
#include "futu_trader/core/clock.hpp"
#include "futu_trader/engine/engine.hpp"
#include "futu_trader/execution/kill_switch.hpp"
#include "futu_trader/execution/rate_limiter.hpp"
#include "futu_trader/infra/metrics_server.hpp"
#include "futu_trader/infra/secrets.hpp"
#include "futu_trader/infra/wal.hpp"
#include "futu_trader/instrument/hk_rules.hpp"
#include "futu_trader/oms/journal_codec.hpp"
#include "futu_trader/oms/live_gate.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/oms/opend_venue.hpp"
#include "futu_trader/oms/push_router.hpp"
#include "futu_trader/opend/client.hpp"
#include "futu_trader/opend/proto_ids.hpp"
#include "futu_trader/opend/quote_decode.hpp"
#include "futu_trader/strategy/strategies.hpp"

namespace futu_trader::app {
namespace {

namespace fs = std::filesystem;
using Log = std::function<void(const std::string&)>;

std::int64_t wallNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr std::int64_t kNsPerDay = 86'400LL * 1'000'000'000LL;
constexpr std::int64_t kHkOffsetNs = 8LL * 3600 * 1'000'000'000LL;

// Start of today in Hong Kong (UTC+8, no DST) as wall-clock ns: earlier intents belong to days the
// broker no longer lists, so they are not restored.
std::int64_t startOfHkDayNs(std::int64_t wallNs) {
  return ((wallNs + kHkOffsetNs) / kNsPerDay) * kNsPerDay - kHkOffsetNs;
}

std::string hkDate(std::int64_t wallNs) {
  const auto days = std::chrono::sys_days{std::chrono::days{(wallNs + kHkOffsetNs) / kNsPerDay}};
  const std::chrono::year_month_day ymd{days};
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", static_cast<int>(ymd.year()),
                static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
  return buf;
}

Log defaultLog() {
  return [](const std::string& line) {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[32];
    std::tm tm{};
    gmtime_r(&now, &tm);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
    std::cerr << stamp << " " << line << "\n";
  };
}

// State directory: created owner-only; an existing one must be ours and not group/world writable.
Result<bool> prepareStateDir(const std::string& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return Error{ErrorCode::kInvalidArg, "cannot create state dir " + dir + ": " + ec.message()};
  }
  struct stat info {};
  if (::stat(dir.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
    return Error{ErrorCode::kInvalidArg, "state dir is not a directory: " + dir};
  }
  if (info.st_uid != ::geteuid()) {
    return Error{ErrorCode::kInvalidArg, "state dir is not owned by the running user: " + dir};
  }
  if ((info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return Error{ErrorCode::kInvalidArg, "state dir must not be group/world writable: " + dir};
  }
  return true;
}

// A text file we make a safety decision from (the promotion log): a regular file we own that others
// cannot write. Not a defence against root, only against mistakes and other users.
Result<std::string> readTrustedText(const std::string& path, std::size_t maxBytes) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return Error{ErrorCode::kInvalidArg, "not a regular file (or a symlink): " + path};
  }
  if (info.st_uid != ::geteuid() || (info.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    return Error{ErrorCode::kInvalidArg,
                 "must be owned by us and not group/world writable: " + path};
  }
  if (static_cast<std::size_t>(info.st_size) > maxBytes) {
    return Error{ErrorCode::kInvalidArg, "file too large: " + path};
  }
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

struct Startup {
  AppConfig cfg;
  Log log;
};

bool isTradePush(std::uint32_t id) {
  return id == opend::protoId::kTrdUpdateOrder || id == opend::protoId::kTrdUpdateOrderFill;
}

instrument::InstrumentTable makeInstruments(const AppConfig& cfg) {
  instrument::InstrumentTable table;
  for (const auto& sym : cfg.symbols) {
    table.add({sym.code, sym.lotSize});
  }
  return table;
}

oms::PreTradeConfig preTradeFrom(const AppConfig& cfg) {
  return {.priceBandBps = cfg.risk.priceBandBps,
          .maxQuoteAgeMs = cfg.risk.maxQuoteAgeMs,
          .maxOrderNotionalMills = cfg.risk.maxOrderNotional,
          .allowShort = cfg.risk.allowShort};
}

RiskConfig limitsFrom(const AppConfig& cfg) {
  return {.maxPositionNotionalMinor = cfg.risk.maxPositionNotional,
          .maxPortfolioNotionalMinor = cfg.risk.maxPortfolioNotional,
          .maxDailyLossMinor = cfg.risk.maxDailyLoss,
          .maxOpenOrders = cfg.risk.maxOpenOrders,
          .concentrationLimit = cfg.risk.concentrationLimit};
}

execution::RateLimitConfig rateFrom(const AppConfig& cfg) {
  return {.maxPerWindow = cfg.rate.maxPerWindow,
          .windowMs = cfg.rate.windowMs,
          .reservedForCancels = cfg.rate.reservedForCancels};
}

// A throwaway OMS over a READ-ONLY view of the real account: bootstrap and reconcile once, to prove
// the account is in a state we understand BEFORE the live gate may approve. It cannot place orders
// (the read-only target is refused by the venue) and its state is discarded.
bool startupReconcileIsClean(const AppConfig& cfg, opend::OpenDClient& client,
                             const oms::PendingLiveApproval& pending, const Log& log) {
  const auto target =
      oms::TradeTarget::realReadOnly(pending, cfg.account.id, opend::TrdMarket::kHK);
  if (!target) {
    log("startup check: " + target.error().message);
    return false;
  }
  oms::OpenDVenue venue(client, target.value());
  SteadyClock clock;
  execution::KillSwitch kill;
  execution::RateLimiter rate(rateFrom(cfg), clock);
  auto instruments = makeInstruments(cfg);
  oms::PreTradeRisk risk(limitsFrom(cfg), preTradeFrom(cfg), kill, instruments);
  portfolio::PositionBook book;
  oms::OmsConfig oc;
  oc.sessionEpoch = "STARTUPCHECK";
  oc.reserveOrders = 1000;
  oms::Oms oms(venue, risk, rate, kill, book, clock, oc);
  const auto boot = oms.bootstrap();
  if (!boot) {
    log("startup check: bootstrap failed: " + boot.error().message);
    return false;
  }
  const auto report = oms.reconcile();
  if (!report.clean() || oms.unresolvedCount() != 0 || kill.tripped()) {
    log("startup check: reconciliation not clean: " +
        (report.error.empty() ? std::to_string(report.drifts.size()) + " drift(s)" : report.error));
    return false;
  }
  return true;
}

}  // namespace

int resetKillSwitch(const AppConfig& config, const std::string& operatorName, const Log& logFn) {
  const Log log = logFn ? logFn : defaultLog();
  if (operatorName.empty()) {
    log("refused: an operator name is required (--operator NAME)");
    return kExitUsage;
  }
  const auto dir = prepareStateDir(config.state.dir);
  if (!dir) {
    log(dir.error().message);
    return kExitRefused;
  }
  execution::KillSwitch kill((fs::path(config.state.dir) / "kill_switch.tripped").string());
  if (!kill.tripped()) {
    log("kill switch is not tripped; nothing to do");
    return kExitOk;
  }
  log("kill switch was tripped: " + kill.reason());
  if (!kill.reset(operatorName)) {
    log("reset refused");
    return kExitRefused;
  }
  log("kill switch reset by " + operatorName);
  return kExitOk;
}

int runTrader(const AppConfig& cfg, RunOptions& opts) {
  const Log log = opts.log ? opts.log : defaultLog();
  const bool real = cfg.mode == Mode::kReal;
  log(std::string("starting in ") + (real ? "REAL" : "SIMULATE") + " mode");

  if (opts.live && !real) {
    log("refused: --live was given but config mode is 'simulate' (they must agree)");
    return kExitUsage;
  }
  if (opts.hardenProcess) {
    const auto h = infra::hardenProcess(true);
    log(std::string("hardening: core dumps ") + (h.coreDumpsDisabled ? "off" : "NOT disabled") +
        ", dumpable " + (h.dumpableCleared ? "cleared" : "NOT cleared") + ", mlockall " +
        (h.memoryLocked ? "ok" : "unavailable"));
  }

  // 1. State directory and the persistent kill switch.
  if (const auto dir = prepareStateDir(cfg.state.dir); !dir) {
    log("refused: " + dir.error().message);
    return kExitRefused;
  }
  const fs::path stateDir(cfg.state.dir);
  execution::KillSwitch kill((stateDir / "kill_switch.tripped").string());
  if (kill.tripped()) {
    log("refused: the kill switch is tripped (" + kill.reason() +
        "). A human must investigate and run: futu_trader --config <file> --reset-kill-switch "
        "--operator <name>");
    return kExitRefused;
  }

  // 2. Write-ahead log: open (repairs a torn tail, refuses a corrupt log) and load past intents.
  const std::string walPath = (stateDir / "orders.wal").string();
  auto wal = infra::Wal::open({walPath});
  if (!wal) {
    log("refused: " + wal.error().message);
    return kExitRefused;
  }
  std::vector<oms::DurableSubmit> pastIntents;
  for (const auto& record : infra::readWal(walPath).records) {
    if (auto submit = oms::decodeSubmit(record)) {
      pastIntents.push_back(std::move(*submit));
    }
  }

  // 3. OpenD.
  opend::ClientConfig cc;
  cc.connection.host = cfg.opend.host;
  cc.connection.port = cfg.opend.port;
  cc.connection.requestTimeout = std::chrono::milliseconds(cfg.opend.requestTimeoutMs);
  cc.connection.allowNonLoopback = cfg.opend.allowNonLoopback;
  opend::OpenDClient client(cc);
  if (const auto session = client.connect(); !session) {
    log("cannot connect to OpenD: " + session.error().message);
    return kExitConnect;
  }
  struct ClientCloser {
    opend::OpenDClient& c;
    ~ClientCloser() { c.close(); }
  } closer{client};

  const auto accounts = client.getAccList();
  if (!accounts) {
    log("cannot list accounts: " + accounts.error().message);
    return kExitConnect;
  }
  const TrdEnv wantEnv = real ? TrdEnv::kReal : TrdEnv::kSimulate;
  const auto account =
      std::find_if(accounts.value().begin(), accounts.value().end(),
                   [&](const opend::TrdAccount& a) { return a.accId == cfg.account.id; });
  if (account == accounts.value().end()) {
    log("refused: account " + std::to_string(cfg.account.id) + " is not reported by OpenD");
    return kExitRefused;
  }
  if (account->env != wantEnv) {
    log(std::string("refused: account ") + std::to_string(cfg.account.id) + " is a " +
        (account->env == TrdEnv::kReal ? "REAL" : "SIMULATE") + " account but config mode is '" +
        (real ? "real" : "simulate") + "'");
    return kExitRefused;
  }
  if (std::find(account->markets.begin(), account->markets.end(),
                static_cast<std::int32_t>(opend::TrdMarket::kHK)) == account->markets.end()) {
    log("refused: the account has no Hong Kong trading authorisation");
    return kExitRefused;
  }

  // 4. Target. SIMULATE needs nothing more. REAL passes the whole live gate.
  std::optional<oms::TradeTarget> target;
  if (!real) {
    target = oms::TradeTarget::simulate(cfg.account.id, opend::TrdMarket::kHK);
  } else {
    auto secret = infra::readSecretFile(cfg.tradePasswordMd5File);
    if (!secret) {
      log("refused: " + secret.error().message);
      return kExitRefused;
    }
    const auto unlocked = client.unlockTrade(secret.value());
    if (!unlocked) {
      log("refused: trade unlock failed: " + unlocked.error().message);
      return kExitRefused;
    }
    auto promotion = readTrustedText(cfg.live.promotionLog, oms::kMaxPromotionLogBytes);
    if (!promotion) {
      log("refused: promotion log: " + promotion.error().message);
      return kExitRefused;
    }
    auto entries = oms::parsePromotionLog(promotion.value());
    if (!entries) {
      log("refused: promotion log is invalid: " + entries.error().message);
      return kExitRefused;
    }
    oms::LiveGateInput gate;
    gate.configPhrase = cfg.live.ackPhrase;
    gate.envVar = opts.liveEnv;
    gate.cliLiveFlag = opts.live;
    gate.promotion = std::move(entries.value());
    gate.requiredCleanDays = cfg.live.requiredCleanDays;
    gate.today = opts.today.empty() ? hkDate(wallNowNs()) : opts.today;
    gate.tradeUnlocked = true;
    gate.configuredAccId = cfg.account.id;
    gate.brokerAccounts = accounts.value();
    const auto pending = oms::LiveGate::approveRealPending(gate);
    if (!pending) {
      log("refused: " + pending.error().message);
      return kExitRefused;
    }
    const bool clean = startupReconcileIsClean(cfg, client, pending.value(), log);
    const auto approval = oms::LiveGate::confirm(pending.value(), clean);
    if (!approval) {
      log("refused: " + approval.error().message);
      return kExitRefused;
    }
    const auto realTarget =
        oms::TradeTarget::real(approval.value(), cfg.account.id, opend::TrdMarket::kHK);
    if (!realTarget) {
      log("refused: " + realTarget.error().message);
      return kExitRefused;
    }
    target = realTarget.value();
    log("live gate passed for account " + std::to_string(cfg.account.id));
  }

  // 5. The trading stack.
  SteadyClock clock;
  auto instruments = makeInstruments(cfg);
  portfolio::PositionBook book;
  execution::RateLimiter rate(rateFrom(cfg), clock);
  oms::PreTradeRisk risk(limitsFrom(cfg), preTradeFrom(cfg), kill, instruments);
  oms::OpenDVenue venue(client, *target);
  oms::OmsConfig oc;
  oc.sessionEpoch = std::to_string(wallNowNs() / 1'000'000);
  oms::Oms oms(venue, risk, rate, kill, book, clock, oc);

  // Write-ahead: the intent is durable before the order can leave the process. The record carries
  // WALL-clock time (the OMS clock is monotonic and means nothing after a restart).
  auto* walPtr = wal.value().get();
  oms.setDurableSubmitSink([walPtr](const oms::DurableSubmit& d) {
    oms::DurableSubmit stamped = d;
    stamped.tsNs = wallNowNs();
    return walPtr->appendDurable(oms::encodeSubmit(stamped));
  });
  oms.setJournalSink([walPtr](const oms::JournalEntry& e) {
    oms::JournalEntry stamped = e;
    stamped.tsNs = wallNowNs();
    walPtr->appendAsync(oms::encodeJournal(stamped));
  });
  const auto restored = oms.restoreIntents(pastIntents, startOfHkDayNs(wallNowNs()));
  if (restored > 0) {
    log("restored " + std::to_string(restored) +
        " order intent(s) from the log; they stay blocked until reconciled");
  }

  oms::PushRouter router(oms, *target);
  engine::EngineConfig ec;
  ec.intentPrefix = "S" + oc.sessionEpoch + "-";
  ec.ringCapacity = cfg.engine.ringCapacity;
  ec.maxQuoteAgeNs = cfg.engine.maxQuoteAgeMs * 1'000'000;
  ec.killFlagPath = (stateDir / "HALT").string();
  ec.reconcileEveryNs = cfg.engine.reconcileEverySec * 1'000'000'000;
  ec.busyPoll = cfg.engine.busyPoll;
  ec.engineCpu = cfg.engine.engineCpu;
  ec.reconcilerCpu = cfg.engine.reconcilerCpu;
  ec.engineRealtimePriority = cfg.engine.realtimePriority;

  std::unique_ptr<strategy::IStrategy> strat;
  if (cfg.strategy.name == "meanrev") {
    strat = std::make_unique<strategy::MeanReversion>(strategy::MeanReversion::Params{
        cfg.strategy.symbol, cfg.strategy.window, cfg.strategy.entryZx10, cfg.strategy.exitZx10,
        cfg.strategy.qty});
  } else if (cfg.strategy.name == "maker") {
    strat = std::make_unique<strategy::PassiveMaker>(strategy::PassiveMaker::Params{
        cfg.strategy.symbol, cfg.strategy.qty, cfg.strategy.cancelAfterQuotes});
  } else {
    strat = std::make_unique<strategy::BuyAndHold>(cfg.strategy.symbol, cfg.strategy.qty);
  }
  engine::Engine engine(oms, *strat, book, clock, kill, ec);

  // Pushes: trade events to the OMS router, quotes to the engine's ring. Runs on the OpenD reader
  // thread, which is the engine's single producer; it never calls the venue.
  opend::QuoteAssembler quotes;
  std::atomic<std::uint64_t> quoteDecodeErrors{0};
  client.setPushHandler([&](const opend::Frame& frame) {
    if (isTradePush(frame.protoId)) {
      router.onFrame(frame);
      return;
    }
    const auto quote = quotes.onFrame(frame, clock.nowNs());
    if (!quote) {
      quoteDecodeErrors.fetch_add(1);
    } else if (quote.value()) {
      engine.onQuote(*quote.value());
    }
  });
  // Losing OpenD means we cannot see fills or cancel: halt (a halt cancels what it can once the
  // link is back, and the persistent kill switch then needs a human).
  client.setConnectionStateHandler([&](bool connected) {
    if (!connected) {
      oms.requestHalt("OpenD connection lost");
    }
  });

  // The reader thread calls the handlers above, which use the OMS, router, engine and quote state
  // declared before this point. Declared AFTER them, this guard is destroyed BEFORE them on every
  // exit path, so the reader is joined while everything it touches is still alive.
  struct ReaderStopper {
    opend::OpenDClient& c;
    ~ReaderStopper() { c.close(); }
  } readerStopper{client};

  std::vector<opend::SecurityRef> securities;
  for (const auto& sym : cfg.symbols) {
    securities.push_back({opend::kQotMarketHkSecurity, sym.code});
  }
  if (const auto sub = client.subscribeAccountPush({cfg.account.id}); !sub) {
    log("cannot subscribe to account pushes: " + sub.error().message);
    return kExitConnect;
  }
  if (const auto sub =
          client.subscribe(securities, {opend::SubType::kBasic, opend::SubType::kOrderBook});
      !sub) {
    log("cannot subscribe to market data: " + sub.error().message);
    return kExitConnect;
  }

  // 6. Learn the account's true state, and refuse to trade unless it is understood.
  if (const auto boot = oms.bootstrap(); !boot) {
    log("cannot bootstrap from the broker: " + boot.error().message);
    return kExitConnect;
  }
  const auto first = oms.reconcile();
  if (!first.complete) {
    log("refused: first reconciliation incomplete: " + first.error);
    return kExitConnect;
  }
  if (!first.drifts.empty() || kill.tripped()) {
    log("refused: first reconciliation found " + std::to_string(first.drifts.size()) +
        " unexplained difference(s); the kill switch is now tripped");
    for (const auto& d : first.drifts) {
      log("  drift: " + d.detail);
    }
    oms.haltAndCancelAll("startup reconciliation found drift");
    return kExitRefused;
  }

  // 7. Observability.
  infra::MetricsRegistry registry;
  bool registered = app::registerOmsMetrics(registry, oms, kill, rate).ok() &&
                    app::registerEngineMetrics(registry, engine).ok() &&
                    app::registerWalMetrics(registry, *walPtr).ok();
  registered =
      registered &&
      registry
          .addCounter("futu_push_foreign_total", "Pushes for another account/env/market.",
                      [&router] { return static_cast<std::uint64_t>(router.foreign()); })
          .ok() &&
      registry
          .addCounter("futu_push_undecodable_total", "Trade pushes that could not be decoded.",
                      [&router] { return static_cast<std::uint64_t>(router.undecodable()); })
          .ok() &&
      registry
          .addCounter("futu_quote_decode_errors_total", "Malformed quote pushes.",
                      [&quoteDecodeErrors] { return quoteDecodeErrors.load(); })
          .ok() &&
      registry
          .addGauge("futu_opend_connected", "1 while the OpenD link is up.",
                    [&client] { return client.isConnected() ? 1.0 : 0.0; })
          .ok();
  if (!registered) {
    log("internal error: metric registration failed");
    return kExitUsage;
  }
  std::unique_ptr<infra::MetricsServer> server;
  RunningInfo info;
  if (cfg.metrics.enabled) {
    server = std::make_unique<infra::MetricsServer>(
        registry,
        [&] {
          return client.isConnected() && !kill.tripped() && !oms.haltPending() &&
                 !engine.stats().engineFailed.load() && !walPtr->failed();
        },
        infra::MetricsServerConfig{cfg.metrics.port});
    const auto port = server->start();
    if (!port) {
      log("cannot start the metrics server: " + port.error().message);
      return kExitRefused;
    }
    info.metricsPort = port.value();
  }

  // 8. Trade.
  engine.start();
  log("trading started (" + cfg.strategy.name + " on " + cfg.strategy.symbol + ")");
  if (opts.onRunning) {
    opts.onRunning(info);
  }
  const auto stopRequested = [&] { return opts.stop != nullptr && opts.stop->load(); };
  while (!stopRequested() && !kill.tripped() && !engine.stats().engineFailed.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(opts.pollIntervalMs));
  }

  // 9. Shutdown. Stop quote handling first so nothing new is ordered, then cancel what rests.
  const bool halted = kill.tripped() || engine.stats().engineFailed.load();
  engine.stop();
  if (halted) {
    log("trading halted: " + (kill.tripped() ? kill.reason() : std::string("engine failure")));
    oms.haltAndCancelAll(kill.tripped() ? kill.reason() : "engine failure");
  } else {
    log("stop requested: cancelling resting orders");
    const auto report = oms.cancelAllLive();
    log("cancel requested for " + std::to_string(report.cancelRequested) + " order(s), " +
        std::to_string(report.cancelFailed) + " failed, " +
        std::to_string(report.unresolvedWithoutVenueId) + " of unknown outcome");
  }
  if (server) {
    server->stop();
  }
  walPtr->flush();
  log("stopped");
  return halted ? kExitHalted : kExitOk;
}

}  // namespace futu_trader::app
