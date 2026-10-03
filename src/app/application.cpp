#include "futu_trader/app/application.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include "futu_trader/app/config_mapping.hpp"
#include "futu_trader/app/link_guard.hpp"
#include "futu_trader/app/live_startup.hpp"
#include "futu_trader/app/log.hpp"
#include "futu_trader/app/promotion.hpp"
#include "futu_trader/app/trading_stack.hpp"
#include "futu_trader/app/trusted_fs.hpp"
#include "futu_trader/app/wall_time.hpp"
#include "futu_trader/infra/metrics_server.hpp"
#include "futu_trader/infra/secrets.hpp"
#include "futu_trader/infra/wal.hpp"

namespace futu_trader::app {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr std::int64_t kEarliestSaneClockNs = 1'735'689'600LL * 1'000'000'000LL;  // 2025-01-01

/**
 * One run of the trading process as a pipeline of stages. Each stage either succeeds or returns the
 * exit code that stops the process; none of them guesses. Members are declared in dependency order,
 * so destruction (the reverse) stops the metrics server first, then the stack (which closes the
 * OpenD link and joins its reader before anything it touches goes away), then the link and files.
 */
class Trader {
 public:
  Trader(const AppConfig& cfg, RunOptions& opts)
      : cfg_(cfg),
        opts_(opts),
        log_(opts.log ? opts.log : defaultLog()),
        real_(cfg.mode == Mode::kReal),
        stateDir_(cfg.state.dir) {}

  int run() {
    using StageFn = std::optional<int> (Trader::*)();
    log_(std::string("starting in ") + (real_ ? "REAL" : "SIMULATE") + " mode");
    for (const StageFn stage : {&Trader::checkPreconditions, &Trader::openState, &Trader::connect,
                                &Trader::startStack, &Trader::startObservability}) {
      if (const auto exitCode = (this->*stage)()) {
        return *exitCode;
      }
    }
    trade();
    return shutdown();
  }

 private:
  using Stage = std::optional<int>;  // an exit code means "stop here"

  int refuse(const std::string& why, int code = kExitRefused) {
    log_("refused: " + why);
    return code;
  }

  Stage checkPreconditions() {
    if (opts_.live && !real_) {
      return refuse("--live was given but config mode is 'simulate' (they must agree)", kExitUsage);
    }
    if (opts_.hardenProcess) {
      const auto h = infra::hardenProcess(true);
      log_(std::string("hardening: core dumps ") + (h.coreDumpsDisabled ? "off" : "NOT disabled") +
           ", dumpable " + (h.dumpableCleared ? "cleared" : "NOT cleared") + ", mlockall " +
           (h.memoryLocked ? "ok" : "unavailable"));
    }
    // A clock that was never set (before NTP) makes "today" meaningless: intents would be judged to
    // be from an old day and not restored, and the live gate would misjudge the promotion log.
    if (wallNowNs() < kEarliestSaneClockNs) {
      return refuse("the system clock is before 2025-01-01; wait for time sync");
    }
    return std::nullopt;
  }

  // State directory, external halt lever, persistent kill switch, write-ahead log.
  Stage openState() {
    if (const auto dir = prepareStateDir(cfg_.state.dir); !dir) {
      return refuse(dir.error().message);
    }
    std::error_code ec;
    if (fs::symlink_status(stateDir_ / "HALT", ec).type() != fs::file_type::not_found) {
      return refuse((stateDir_ / "HALT").string() +
                    " exists (the external halt lever); remove it deliberately, then start again");
    }
    kill_ = std::make_unique<execution::KillSwitch>((stateDir_ / "kill_switch.tripped").string());
    if (kill_->tripped()) {
      return refuse("the kill switch is tripped (" + kill_->reason() +
                    "). A human must investigate and run: futu_trader --config <file> "
                    "--reset-kill-switch --operator <name>");
    }
    // Opening repairs a torn tail, refuses a corrupt log and takes the single-writer lock.
    const std::string walPath = (stateDir_ / "orders.wal").string();
    auto wal = infra::Wal::open({walPath});
    if (!wal) {
      return refuse(wal.error().message);
    }
    wal_ = std::move(wal.value());
    for (const auto& record : infra::readWal(walPath).records) {
      if (record.empty() || record[0] != 'S') {
        continue;  // audit entries ('J'); only submit records matter for restart safety
      }
      auto submit = oms::decodeSubmit(record);
      if (!submit) {
        // A checksum-valid submit record we cannot read (newer format?) is an order intent we
        // would silently forget: the one thing the log exists to prevent.
        return refuse("the write-ahead log holds a submit record this build cannot decode");
      }
      pastIntents_.push_back(std::move(*submit));
    }
    return std::nullopt;
  }

  Stage connect() {
    client_ = std::make_unique<opend::OpenDClient>(clientConfig(cfg_));
    guard_ = std::make_unique<LinkGuard>(*client_);
    if (const auto session = client_->connect(); !session) {
      log_("cannot connect to OpenD: " + session.error().message);
      return kExitConnect;
    }
    const auto accounts = client_->getAccList();
    if (!accounts) {
      log_("cannot list accounts: " + accounts.error().message);
      return kExitConnect;
    }
    if (const auto ok = verifyAccount(cfg_, accounts.value()); !ok) {
      return refuse(ok.error().message);
    }
    accounts_ = accounts.value();
    return std::nullopt;
  }

  Stage startStack() {
    const auto target = selectTarget(cfg_, opts_, *client_, *guard_, accounts_, log_);
    if (!target) {
      return refuse(target.error().message);
    }
    stack_ = std::make_unique<TradingStack>(cfg_, stateDir_, *client_, *guard_, target.value(),
                                            *kill_, *wal_, pastIntents_, log_);
    return stack_->prepare();
  }

  Stage startObservability() {
    if (!stack_->registerMetrics(registry_)) {
      log_("internal error: metric registration failed");
      return kExitUsage;
    }
    if (cfg_.metrics.enabled) {
      server_ = std::make_unique<infra::MetricsServer>(
          registry_, [this] { return stack_->ready(); },
          infra::MetricsServerConfig{cfg_.metrics.port});
      const auto port = server_->start();
      if (!port) {
        log_("cannot start the metrics server: " + port.error().message);
        return kExitRefused;
      }
      running_.metricsPort = port.value();
    }
    return std::nullopt;
  }

  bool stopRequested() const { return opts_.stop != nullptr && opts_.stop->load(); }
  bool haltedNow() const {
    return kill_->tripped() || stack_->engine().stats().engineFailed.load();
  }

  void trade() {
    sessionStart_ = Clock::now();
    stack_->engine().start();
    log_("trading started (" + cfg_.strategy.name + " on " + cfg_.strategy.symbol + ")");
    if (opts_.onRunning) {
      opts_.onRunning(running_);
    }
    while (!stopRequested() && !haltedNow()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(opts_.pollIntervalMs));
    }
  }

  // Keeps trying until the deadline: the link may be down (a halt often IS the link going down) or
  // an order of unknown outcome may need a reconciliation to learn its broker id before it can be
  // cancelled. Exiting after one failed pass would leave orders resting with nothing in the log.
  bool cancelUntilSettled(bool halted, const std::string& reason) {
    oms::Oms& oms = stack_->oms();
    const auto deadline = Clock::now() + std::chrono::milliseconds(opts_.shutdownDeadlineMs);
    while (true) {
      const auto report = halted ? oms.haltAndCancelAll(reason) : oms.cancelAllLive();
      log_("cancel requested for " + std::to_string(report.cancelRequested) + " order(s), " +
           std::to_string(report.cancelFailed) + " failed, " +
           std::to_string(report.unresolvedWithoutVenueId) + " of unknown outcome");
      if (report.cancelFailed == 0 && report.unresolvedWithoutVenueId == 0) {
        return true;
      }
      if (Clock::now() >= deadline) {
        log_("WARNING: orders may still be resting at the broker: check the broker app NOW");
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(opts_.shutdownRetryMs));
      static_cast<void>(oms.reconcile());  // learns broker ids of unknown orders; refreshes states
    }
  }

  // SIMULATE sessions leave evidence for the live gate: one line per day, dirty is sticky. A clean
  // day needs proof the strategy actually ran: an idle process earns nothing.
  void recordPromotionEvidence(bool halted, bool settled) {
    if (real_ || cfg_.live.promotionLog.empty()) {
      return;
    }
    const auto& stats = stack_->engine().stats();
    const std::array<std::pair<bool, const char*>, 6> checks{{
        {!settled, "cancels incomplete"},
        {halted, "halted"},
        {stats.reconcileProblems.load() != 0, "reconcile problems"},
        {stack_->oms().anomalyCount() != 0, "OMS anomalies"},
        {stack_->oms().unresolvedCount() != 0, "unresolved orders"},
        {stack_->router().foreign() != 0, "foreign pushes"},
    }};
    std::string why;
    for (const auto& [bad, what] : checks) {
      if (bad) {
        why += std::string(why.empty() ? "" : ", ") + what;
      }
    }
    if (stack_->oms().anomalyCount() != 0) {
      for (const auto& entry : stack_->oms().journal()) {
        if (entry.kind == oms::JournalKind::kAnomaly) {
          log_("OMS anomaly: " + entry.clOrdId + ": " + entry.detail);
        }
      }
    }
    const bool problems = !why.empty();
    const auto minutes =
        std::chrono::duration_cast<std::chrono::minutes>(Clock::now() - sessionStart_).count();
    if (!problems && stats.processed.load() == 0) {
      log_("promotion log: no quotes were processed this session, nothing recorded");
    } else if (problems || minutes >= cfg_.live.minSessionMinutes) {
      const std::string date = opts_.today.empty() ? hkDate(wallNowNs()) : opts_.today;
      const auto recorded = recordPromotionDay(cfg_.live.promotionLog, date, !problems);
      log_(recorded ? "promotion log: " + date + (problems ? " dirty (" + why + ")" : " clean")
                    : "promotion log NOT updated: " + recorded.error().message);
    } else {
      log_("promotion log: session shorter than live.min_session_minutes, nothing recorded");
    }
  }

  // Stop quote handling first so nothing new is ordered, then cancel what rests.
  int shutdown() {
    const bool halted = haltedNow();
    const std::string reason = kill_->tripped() ? kill_->reason() : std::string("engine failure");
    stack_->engine().stop();
    log_(halted ? "trading halted: " + reason : "stop requested: cancelling resting orders");
    const bool settled = cancelUntilSettled(halted, reason);
    recordPromotionEvidence(halted, settled);
    if (server_) {
      server_->stop();
    }
    wal_->flush();
    log_("stopped");
    if (!settled) {
      return kExitOrdersMayRest;
    }
    return halted ? kExitHalted : kExitOk;
  }

  const AppConfig& cfg_;
  RunOptions& opts_;
  const Log log_;
  const bool real_;
  const fs::path stateDir_;

  std::unique_ptr<execution::KillSwitch> kill_;
  std::unique_ptr<infra::Wal> wal_;
  std::vector<oms::DurableSubmit> pastIntents_;
  std::unique_ptr<opend::OpenDClient> client_;
  std::unique_ptr<LinkGuard> guard_;
  std::vector<opend::TrdAccount> accounts_;
  std::unique_ptr<TradingStack> stack_;
  infra::MetricsRegistry registry_;
  std::unique_ptr<infra::MetricsServer> server_;  // last: stops serving before anything it reads
  RunningInfo running_;
  Clock::time_point sessionStart_;
};

}  // namespace

int runTrader(const AppConfig& cfg, RunOptions& opts) { return Trader(cfg, opts).run(); }

int resetKillSwitch(const AppConfig& config, const std::string& operatorName, const Log& logFn) {
  const Log log = logFn ? logFn : defaultLog();
  if (operatorName.empty()) {
    log("refused: an operator name is required (--operator NAME)");
    return kExitUsage;
  }
  if (const auto dir = prepareStateDir(config.state.dir); !dir) {
    log(dir.error().message);
    return kExitRefused;
  }
  const fs::path stateDir(config.state.dir);
  execution::KillSwitch kill((stateDir / "kill_switch.tripped").string());
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
  // The audit trail lives on disk next to the state it concerns, not only in a terminal.
  std::ofstream audit((stateDir / "audit.log").string(), std::ios::app);
  audit << hkDate(wallNowNs()) << " kill switch reset by " << operatorName << "\n";
  return kExitOk;
}

}  // namespace futu_trader::app
