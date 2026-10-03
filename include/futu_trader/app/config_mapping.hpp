#pragma once

#include <filesystem>
#include <string>

#include "futu_trader/app/config.hpp"
#include "futu_trader/engine/engine.hpp"
#include "futu_trader/execution/rate_limiter.hpp"
#include "futu_trader/instrument/hk_rules.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/oms/pre_trade.hpp"
#include "futu_trader/opend/client.hpp"
#include "futu_trader/strategy/strategy_factory.hpp"

namespace futu_trader::app {

/**
 * The one place where the validated AppConfig is translated into each component's own config.
 * Components never see AppConfig, and the startup check and the live stack cannot drift apart
 * because both build their limits through these functions.
 */
instrument::InstrumentTable makeInstruments(const AppConfig& cfg);
oms::PreTradeConfig preTradeConfig(const AppConfig& cfg);
RiskConfig riskConfig(const AppConfig& cfg);
execution::RateLimitConfig rateConfig(const AppConfig& cfg);
opend::ClientConfig clientConfig(const AppConfig& cfg);
strategy::StrategySpec strategySpec(const AppConfig& cfg);
oms::OmsConfig omsConfig(const AppConfig& cfg, std::string sessionEpoch);
engine::EngineConfig engineConfig(const AppConfig& cfg, const std::string& sessionEpoch,
                                  const std::filesystem::path& stateDir);

}  // namespace futu_trader::app
