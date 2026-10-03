#include "futu_trader/app/config_mapping.hpp"

namespace futu_trader::app {

instrument::InstrumentTable makeInstruments(const AppConfig& cfg) {
  instrument::InstrumentTable table;
  for (const auto& sym : cfg.symbols) {
    table.add({sym.code, sym.lotSize});
  }
  return table;
}

oms::PreTradeConfig preTradeConfig(const AppConfig& cfg) {
  return {.priceBandBps = cfg.risk.priceBandBps,
          .maxQuoteAgeMs = cfg.risk.maxQuoteAgeMs,
          .maxOrderNotionalMills = cfg.risk.maxOrderNotional,
          .allowShort = cfg.risk.allowShort};
}

RiskConfig riskConfig(const AppConfig& cfg) {
  return {.maxPositionNotionalMinor = cfg.risk.maxPositionNotional,
          .maxPortfolioNotionalMinor = cfg.risk.maxPortfolioNotional,
          .maxDailyLossMinor = cfg.risk.maxDailyLoss,
          .maxOpenOrders = cfg.risk.maxOpenOrders,
          .concentrationLimit = cfg.risk.concentrationLimit};
}

execution::RateLimitConfig rateConfig(const AppConfig& cfg) {
  return {.maxPerWindow = cfg.rate.maxPerWindow,
          .windowMs = cfg.rate.windowMs,
          .reservedForCancels = cfg.rate.reservedForCancels};
}

opend::ClientConfig clientConfig(const AppConfig& cfg) {
  opend::ClientConfig cc;
  cc.connection.host = cfg.opend.host;
  cc.connection.port = cfg.opend.port;
  cc.connection.requestTimeout = std::chrono::milliseconds(cfg.opend.requestTimeoutMs);
  cc.connection.allowNonLoopback = cfg.opend.allowNonLoopback;
  return cc;
}

strategy::StrategySpec strategySpec(const AppConfig& cfg) {
  const auto& s = cfg.strategy;
  strategy::StrategySpec spec;
  spec.name = s.name;
  spec.symbol = s.symbol;
  spec.qty = s.qty;
  spec.window = s.window;
  spec.entryZx10 = s.entryZx10;
  spec.exitZx10 = s.exitZx10;
  spec.cancelAfterQuotes = s.cancelAfterQuotes;
  return spec;
}

oms::OmsConfig omsConfig(const AppConfig& cfg, std::string sessionEpoch) {
  oms::OmsConfig oc;
  oc.sessionEpoch = std::move(sessionEpoch);
  oc.cashToleranceMills = cfg.engine.cashTolerance;
  return oc;
}

engine::EngineConfig engineConfig(const AppConfig& cfg, const std::string& sessionEpoch,
                                  const std::filesystem::path& stateDir) {
  engine::EngineConfig ec;
  ec.intentPrefix = "S" + sessionEpoch + "-";  // distinct per process: see OmsContext::setKeyPrefix
  ec.ringCapacity = cfg.engine.ringCapacity;
  ec.maxQuoteAgeNs = cfg.engine.maxQuoteAgeMs * 1'000'000;
  ec.killFlagPath = (stateDir / "HALT").string();
  ec.reconcileEveryNs = cfg.engine.reconcileEverySec * 1'000'000'000;
  ec.busyPoll = cfg.engine.busyPoll;
  ec.engineCpu = cfg.engine.engineCpu;
  ec.reconcilerCpu = cfg.engine.reconcilerCpu;
  ec.engineRealtimePriority = cfg.engine.realtimePriority;
  return ec;
}

}  // namespace futu_trader::app
