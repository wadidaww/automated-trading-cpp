#pragma once

#include "futu_trader/core/result.hpp"
#include "futu_trader/engine/engine.hpp"
#include "futu_trader/execution/kill_switch.hpp"
#include "futu_trader/execution/rate_limiter.hpp"
#include "futu_trader/infra/metrics.hpp"
#include "futu_trader/infra/wal.hpp"
#include "futu_trader/oms/oms.hpp"

namespace futu_trader::app {

/**
 * Exposes the components' own counters (no copies, read at scrape time). Every object passed in
 * must outlive the registry's last scrape. These names are the contract that the alert rules in
 * ops/ depend on: change one and ops/prometheus/alerts.yml must change with it.
 */
Result<bool> registerEngineMetrics(infra::MetricsRegistry& registry, const engine::Engine& engine);
Result<bool> registerOmsMetrics(infra::MetricsRegistry& registry, oms::Oms& oms,
                                execution::KillSwitch& kill, execution::RateLimiter& rate);
Result<bool> registerWalMetrics(infra::MetricsRegistry& registry, const infra::Wal& wal);

}  // namespace futu_trader::app
