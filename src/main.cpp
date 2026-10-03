// futu_trader: the trading process. See docs/runbook.md.
//
//   futu_trader --config FILE [--live]
//   futu_trader --config FILE --reset-kill-switch --operator NAME
//   futu_trader --health-check
//
// Exit codes: 0 stopped cleanly; 2 usage/config error; 3 a safety condition refused startup;
// 4 OpenD unreachable or a broker step failed; 6 trading halted while running.

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "futu_trader/app/application.hpp"

namespace {

std::atomic<bool> g_stop{false};

extern "C" void onSignal(int) { g_stop.store(true); }

void usage() {
  std::cerr << "usage: futu_trader --config FILE [--live]\n"
               "       futu_trader --config FILE --reset-kill-switch --operator NAME\n"
               "       futu_trader --health-check\n";
}

}  // namespace

int main(int argc, char** argv) {
  using namespace futu_trader::app;
  std::string configPath;
  std::string operatorName;
  bool live = false;
  bool resetKill = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--health-check") {
      std::cout << "ok\n";
      return 0;
    }
    if (arg == "--config" && i + 1 < argc) {
      configPath = argv[++i];
    } else if (arg == "--operator" && i + 1 < argc) {
      operatorName = argv[++i];
    } else if (arg == "--live") {
      live = true;
    } else if (arg == "--reset-kill-switch") {
      resetKill = true;
    } else {
      usage();
      return kExitUsage;
    }
  }
  if (configPath.empty()) {
    usage();
    return kExitUsage;
  }
  const auto config = loadConfigFile(configPath);
  if (!config) {
    std::cerr << config.error().message << "\n";
    return kExitUsage;
  }
  if (resetKill) {
    return resetKillSwitch(config.value(), operatorName, nullptr);
  }

  struct sigaction sa {};
  sa.sa_handler = onSignal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);

  RunOptions options;
  options.live = live;
  if (const char* env = std::getenv("FUTU_LIVE_TRADING")) {
    options.liveEnv = env;
  }
  options.stop = &g_stop;
  return runTrader(config.value(), options);
}
