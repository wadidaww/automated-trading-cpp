// futu_backtest: replay quotes through the real OMS/risk/portfolio against a simulated broker.
//
//   futu_backtest [--synthetic --seed N --count N --size N | --csv FILE | --log FILE]
//                 [--strategy meanrev|buyhold|random|maker|peeker] [--lot N]
//                 [--commission-bps X] [--platform-fee HKD] [--max-daily-loss HKD]
//                 [--report FILE]
//                 [--check-determinism] [--check-lookahead] [--allow-no-trades]
//                 [--golden FILE | --update-golden FILE]
//
// Exit codes: 0 ok, 2 bad usage/input, 3 non-deterministic, 4 golden mismatch,
//             5 OMS disagrees with the broker (reconciliation not clean),
//             6 the strategy never traded (unless --allow-no-trades), 7 lookahead detected.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>

#include "futu_trader/backtest/runner.hpp"
#include "futu_trader/backtest/synthetic.hpp"
#include "futu_trader/data/csv.hpp"
#include "futu_trader/data/event_log.hpp"
#include "futu_trader/data/validate.hpp"
#include "futu_trader/strategy/strategies.hpp"

using namespace futu_trader;

namespace {

struct Options {
  std::string csvPath;
  std::string logPath;
  std::string strategyName{"meanrev"};
  std::string reportPath;
  std::string goldenPath;
  std::string updateGoldenPath;
  bool checkDeterminism{false};
  bool checkLookahead{false};
  bool allowNoTrades{false};
  std::int64_t lot{100};
  double commissionBps{0.0};
  Money platformFeeMills{0};
  Money maxDailyLossMills{0};  // 0 = keep the default
  bool brokerFeesGiven{false};
  backtest::SyntheticConfig synth;
};

std::string fixed(double value) {
  if (std::isnan(value) || std::isinf(value)) {
    return "null";  // not representable in JSON, and "0" would be a lie
  }
  std::array<char, 64> buf{};
  std::snprintf(buf.data(), buf.size(), "%.6f", value);
  return buf.data();
}

std::string jsonString(const std::string& text) {
  return std::string(1, '"') + text + std::string(1, '"');
}
std::string yesNo(bool value) { return value ? "true" : "false"; }

std::string reportJson(const backtest::BacktestResult& r, const std::string& strategyName,
                       const std::string& symbol, std::size_t events, bool brokerFeesGiven) {
  std::vector<std::string> warnings;
  if (!brokerFeesGiven) {
    warnings.emplace_back(
        "broker commission and platform fee are 0: only statutory charges are modelled, so "
        "costs are understated versus a real account");
  }
  if (!r.perf.ratiosReliable) {
    warnings.emplace_back("fewer than 250 return periods: Sharpe/Sortino/Calmar are not reliable");
  }
  if (r.submits == 0) {
    warnings.emplace_back("the strategy submitted no orders");
  }
  const auto ci = backtest::bootstrapSharpeCi(r.periodReturns, r.periodsPerYear, 1000, 1);

  std::ostringstream hash;
  hash << std::hex << r.journalHash;
  std::string positions = "{";
  bool first = true;
  for (const auto& [sym, qty] : r.finalPositions) {
    positions += (first ? "" : ", ") + jsonString(sym) + ": " + std::to_string(qty);
    first = false;
  }
  positions += "}";
  std::string warningList = "[";
  for (std::size_t i = 0; i < warnings.size(); ++i) {
    warningList += (i == 0 ? "" : ", ") + jsonString(warnings[i]);
  }
  warningList += "]";

  const std::vector<std::pair<std::string, std::string>> fields{
      {"strategy", jsonString(strategyName)},
      {"symbol", jsonString(symbol)},
      {"events", std::to_string(events)},
      {"journal_hash", jsonString(hash.str())},
      {"initial_equity_mills", std::to_string(r.initialEquity)},
      {"final_equity_mills", std::to_string(r.finalEquity)},
      {"final_liquidation_equity_mills", std::to_string(r.finalLiquidationEquity)},
      {"total_fees_mills", std::to_string(r.totalFees)},
      {"final_positions", positions},
      {"fills", std::to_string(r.fills.size())},
      {"orders", std::to_string(r.orders.size())},
      {"submits", std::to_string(r.submits)},
      {"accepted", std::to_string(r.accepted)},
      {"risk_rejects", std::to_string(r.riskRejects)},
      {"venue_rejects", std::to_string(r.venueRejects)},
      {"closed_trades", std::to_string(r.trades.count)},
      {"reconciles", std::to_string(r.reconciles)},
      {"reconcile_drifts", std::to_string(r.reconcileDrifts)},
      {"final_reconcile_clean", yesNo(r.finalReconcileClean)},
      {"kill_switch_tripped", yesNo(r.killSwitchTripped)},
      {"periods_per_year", fixed(r.periodsPerYear)},
      {"return_periods", std::to_string(r.perf.periods)},
      {"ratios_reliable", yesNo(r.perf.ratiosReliable)},
      {"total_return", fixed(r.perf.totalReturn)},
      {"sharpe", fixed(r.perf.sharpe)},
      {"sharpe_ci95_lo", ci.valid ? fixed(ci.lo) : "null"},
      {"sharpe_ci95_hi", ci.valid ? fixed(ci.hi) : "null"},
      {"sortino", fixed(r.perf.sortino)},
      {"max_drawdown", fixed(r.perf.maxDrawdown)},
      {"hit_rate_net_of_fees", fixed(r.trades.hitRate)},
      {"profit_factor_net_of_fees", fixed(r.trades.profitFactor)},
      {"exposure", fixed(r.exposureFraction)},
      {"turnover", fixed(r.turnover)},
      {"warnings", warningList},
  };
  std::ostringstream out;
  out << "{\n";
  for (std::size_t i = 0; i < fields.size(); ++i) {
    out << "  " << jsonString(fields[i].first) << ": " << fields[i].second
        << (i + 1 < fields.size() ? ",\n" : "\n");
  }
  out << "}\n";
  return out.str();
}

bool readFile(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  out = buf.str();
  return true;
}

std::unique_ptr<strategy::IStrategy> makeStrategy(const std::string& name,
                                                  const std::string& symbol,
                                                  const std::vector<QuoteEvent>* data) {
  if (name == "meanrev") {
    strategy::MeanReversion::Params p;
    p.symbol = symbol;
    p.window = 60;
    p.entryZx10 = 15;
    p.exitZx10 = 0;
    p.qty = 100;
    return std::make_unique<strategy::MeanReversion>(p);
  }
  if (name == "buyhold") {
    return std::make_unique<strategy::BuyAndHold>(symbol, 100);
  }
  if (name == "random") {
    return std::make_unique<strategy::RandomTrader>(symbol, 100, 25);
  }
  if (name == "maker") {
    strategy::PassiveMaker::Params p;
    p.symbol = symbol;
    return std::make_unique<strategy::PassiveMaker>(p);
  }
  if (name == "peeker") {  // the cheating canary: --check-lookahead must reject it
    return std::make_unique<strategy::FuturePeeker>(symbol, 100, data);
  }
  return nullptr;
}

int usage(const std::string& why) {
  std::cerr << "futu_backtest: " << why << "\n"
            << "usage: futu_backtest [--synthetic --seed N --count N --size N | --csv FILE | "
               "--log FILE]\n"
            << "         [--strategy meanrev|buyhold|random|maker|peeker] [--lot N]\n"
            << "         [--commission-bps X] [--platform-fee HKD] [--max-daily-loss HKD]\n"
            << "         [--report FILE]\n"
            << "         [--check-determinism] [--check-lookahead] [--allow-no-trades]\n"
            << "         [--golden FILE | --update-golden FILE]\n";
  return 2;
}

// Returns an error message, or empty on success.
std::string parseArgs(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    bool missingValue = false;
    // Consumes the next argument as this option's value when `arg` is `name`.
    const auto takes = [&](const char* name, std::string& dst) {
      if (arg != name) {
        return false;
      }
      if (i + 1 >= argc) {
        missingValue = true;
        return false;
      }
      dst = argv[++i];
      return true;
    };
    std::string v;
    if (arg == "--synthetic") {
      continue;
    }
    if (arg == "--check-determinism") {
      opt.checkDeterminism = true;
      continue;
    }
    if (arg == "--check-lookahead") {
      opt.checkLookahead = true;
      continue;
    }
    if (arg == "--allow-no-trades") {
      opt.allowNoTrades = true;
      continue;
    }
    if (takes("--csv", opt.csvPath) || takes("--log", opt.logPath) ||
        takes("--strategy", opt.strategyName) || takes("--report", opt.reportPath) ||
        takes("--golden", opt.goldenPath) || takes("--update-golden", opt.updateGoldenPath)) {
      continue;
    }
    if (takes("--seed", v)) {
      opt.synth.seed = std::strtoull(v.c_str(), nullptr, 10);
      continue;
    }
    if (takes("--count", v)) {
      opt.synth.count = std::strtoull(v.c_str(), nullptr, 10);
      continue;
    }
    if (takes("--size", v)) {
      opt.synth.size = std::strtoll(v.c_str(), nullptr, 10);
      continue;
    }
    if (takes("--lot", v)) {
      opt.lot = std::strtoll(v.c_str(), nullptr, 10);
      continue;
    }
    if (takes("--commission-bps", v)) {
      opt.commissionBps = std::strtod(v.c_str(), nullptr);
      opt.brokerFeesGiven = true;
      continue;
    }
    if (takes("--max-daily-loss", v)) {
      const auto loss = data::parsePriceMills(v);
      if (!loss || loss.value() <= 0) {
        return "bad --max-daily-loss '" + v + "'";
      }
      opt.maxDailyLossMills = loss.value();
      continue;
    }
    if (takes("--platform-fee", v)) {
      const auto fee = data::parsePriceMills(v);
      if (!fee) {
        return "bad --platform-fee '" + v + "'";
      }
      opt.platformFeeMills = fee.value();
      opt.brokerFeesGiven = true;
      continue;
    }
    return missingValue ? "option " + arg + " needs a value" : "unknown option " + arg;
  }
  return {};
}

}  // namespace

int run(int argc, char** argv) {
  Options opt;
  if (const std::string bad = parseArgs(argc, argv, opt); !bad.empty()) {
    return usage(bad);
  }
  if (opt.lot <= 0) {
    return usage("--lot must be positive");
  }

  std::vector<QuoteEvent> events;
  if (!opt.csvPath.empty()) {
    std::string text;
    if (!readFile(opt.csvPath, text)) {
      return usage("cannot read the CSV file");
    }
    auto parsed = data::parseQuotesCsv(text);
    if (!parsed) {
      std::cerr << "futu_backtest: " << parsed.error().message << "\n";
      return 2;
    }
    events = std::move(parsed.value());
  } else if (!opt.logPath.empty()) {
    std::ifstream in(opt.logPath, std::ios::binary);
    if (!in) {
      return usage("cannot read the event log");
    }
    auto loaded = data::readEventLog(in);
    if (loaded.status != data::LogStatus::kOk) {
      std::cerr << "futu_backtest: event log problem (status " << static_cast<int>(loaded.status)
                << "); refusing to use partial data\n";
      return 2;
    }
    events = std::move(loaded.quotes);
  } else {
    events = backtest::generateSyntheticQuotes(opt.synth);
  }
  if (events.empty()) {
    return usage("no market data");
  }
  // A checksum proves a log is intact, not that its contents make sense.
  if (const auto valid = data::validateQuotes(events); !valid) {
    std::cerr << "futu_backtest: " << valid.error().message << "\n";
    return 2;
  }
  // Strategies trade one symbol. Deriving it from the data (and refusing mixed data) means a
  // symbol mismatch can never silently produce an empty "successful" backtest.
  const std::string symbol = events.front().symbol;
  std::set<std::string> symbols;
  for (const auto& e : events) {
    symbols.insert(e.symbol);
  }
  if (symbols.size() != 1) {
    return usage("the data contains " + std::to_string(symbols.size()) +
                 " symbols; single-symbol backtests only");
  }
  if (makeStrategy(opt.strategyName, symbol, &events) == nullptr) {
    return usage("unknown strategy '" + opt.strategyName + "'");
  }

  auto config = backtest::defaultBacktestConfig();
  config.instruments = {{symbol, opt.lot}};
  config.venue.fees.commissionPpb =
      static_cast<std::int64_t>(std::llround(opt.commissionBps * 100'000.0));
  config.venue.fees.platformFeeMills = opt.platformFeeMills;
  if (opt.maxDailyLossMills > 0) {
    config.limits.maxDailyLossMinor = opt.maxDailyLossMills;
  }

  auto strat = makeStrategy(opt.strategyName, symbol, &events);
  const auto result = backtest::runBacktest(config, events, *strat);
  if (!result.dataError.empty()) {
    std::cerr << "futu_backtest: " << result.dataError << "\n";
    return 2;
  }

  if (opt.checkDeterminism) {
    auto again = makeStrategy(opt.strategyName, symbol, &events);
    const auto second = backtest::runBacktest(config, events, *again);
    if (second.journalHash != result.journalHash || second.finalEquity != result.finalEquity) {
      std::cerr << "futu_backtest: NON-DETERMINISTIC: two identical runs disagree\n";
      return 3;
    }
  }

  const std::string json =
      reportJson(result, opt.strategyName, symbol, events.size(), opt.brokerFeesGiven);
  std::cout << json;
  if (!opt.reportPath.empty()) {
    std::ofstream(opt.reportPath) << json;
  }
  if (!opt.brokerFeesGiven) {
    std::cerr << "futu_backtest: NOTE: no broker commission/platform fee configured (statutory "
                 "charges only)\n";
  }
  if (!result.perf.ratiosReliable) {
    std::cerr << "futu_backtest: NOTE: only " << result.perf.periods
              << " return periods: risk-adjusted ratios are not reliable\n";
  }

  if (!result.finalReconcileClean || result.reconcileDrifts != 0) {
    std::cerr << "futu_backtest: the OMS disagrees with the simulated broker: " << result.firstDrift
              << "\n";
    return 5;
  }
  if (result.submits == 0 && !opt.allowNoTrades) {
    std::cerr << "futu_backtest: the strategy never traded; that is almost always a setup error "
                 "(wrong symbol, warm-up longer than the data, ...). Pass --allow-no-trades if "
                 "it is expected.\n";
    return 6;
  }
  if (opt.checkLookahead) {
    const auto report = backtest::detectLookahead(
        config, events,
        [&](const std::vector<QuoteEvent>& data) {
          return makeStrategy(opt.strategyName, symbol, &data);
        },
        6);
    if (!report.consistent) {
      std::cerr << "futu_backtest: LOOKAHEAD SUSPECTED: " << report.firstMismatch << "\n";
      return 7;
    }
    std::cerr << "futu_backtest: lookahead check passed (" << report.cutsChecked << " cut points, "
              << report.comparedDecisions << " decisions compared)\n";
  }
  if (!opt.updateGoldenPath.empty()) {
    std::ofstream(opt.updateGoldenPath) << json;
    std::cerr << "futu_backtest: wrote golden file " << opt.updateGoldenPath << "\n";
    return 0;
  }
  if (!opt.goldenPath.empty()) {
    std::string expected;
    if (!readFile(opt.goldenPath, expected)) {
      std::cerr << "futu_backtest: cannot read golden file " << opt.goldenPath << "\n";
      return 2;
    }
    if (expected != json) {
      std::cerr << "futu_backtest: GOLDEN MISMATCH against " << opt.goldenPath
                << ". If the change is intended, regenerate with --update-golden and review the "
                   "diff.\n";
      return 4;
    }
  }
  return 0;
}

int main(int argc, char** argv) {
  // Anything thrown below (allocation failure, an unexpected library error) must become a clear
  // message and a failure exit code, not an abort with no explanation.
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "futu_backtest: fatal: " << error.what() << "\n";
  } catch (...) {
    std::cerr << "futu_backtest: fatal: unknown error\n";
  }
  return 2;
}
