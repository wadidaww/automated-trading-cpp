#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "futu_trader/app/config.hpp"

namespace futu_trader::app {

/** Process exit codes. Anything non-zero means "do not assume trading happened as intended". */
enum ExitCode : std::uint8_t {
  kExitOk = 0,       // stopped on request; resting orders were cancelled
  kExitUsage = 2,    // bad command line or config
  kExitRefused = 3,  // a safety condition refused startup (kill switch, live gate, corrupt WAL...)
  kExitConnect = 4,  // OpenD unreachable or the broker rejected a startup step
  kExitHalted = 6,   // trading halted while running (kill switch tripped); orders were cancelled
};

struct RunningInfo {
  std::uint16_t metricsPort{0};  // 0 if metrics are disabled
};

struct RunOptions {
  bool live{false};     // --live
  std::string liveEnv;  // value of FUTU_LIVE_TRADING (read by main, passed in for testability)
  std::atomic<bool>* stop{nullptr};  // set (e.g. by a signal handler) to request a graceful stop
  std::string today;         // YYYY-MM-DD override for tests; empty = Hong Kong date from the clock
  bool hardenProcess{true};  // RLIMIT_CORE=0, PR_SET_DUMPABLE=0, best-effort mlockall
  std::function<void(const std::string&)> log;        // default: stderr with a timestamp
  std::function<void(const RunningInfo&)> onRunning;  // called once the engine is trading
  std::int64_t pollIntervalMs{100};
};

/**
 * The trading process. Startup refuses (and never trades) unless, in this order: the persistent
 * kill switch is clear, the write-ahead log is intact, OpenD answers, the configured account exists
 * with the environment the config asks for, (REAL only) the full live gate passes including a clean
 * read-only startup reconciliation, the OMS bootstraps from the broker and a first reconciliation
 * is clean. Intents logged by a previous run are restored first, so a crash cannot cause a
 * duplicate.
 *
 * Runs until `opts.stop` is set or trading halts. Blocking; returns an ExitCode.
 */
int runTrader(const AppConfig& cfg, RunOptions& opts);

/** Clears the persistent kill switch on a human's say-so (the operator name is audited). */
int resetKillSwitch(const AppConfig& config, const std::string& operatorName,
                    const std::function<void(const std::string&)>& log);

}  // namespace futu_trader::app
