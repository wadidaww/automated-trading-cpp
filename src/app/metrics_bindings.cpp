#include "futu_trader/app/metrics_bindings.hpp"

#include <array>
#include <string>

namespace futu_trader::app {
namespace {

using infra::MetricsRegistry;

// Chains registrations; stops at the first failure so a bad name is never silently half-applied.
struct Registrar {
  MetricsRegistry& registry;
  Result<bool> status{true};

  void counter(const std::string& name, const std::string& help, MetricsRegistry::CounterFn fn,
               infra::Labels labels = {}) {
    if (status) {
      status = registry.addCounter(name, help, std::move(fn), std::move(labels));
    }
  }
  void gauge(const std::string& name, const std::string& help, MetricsRegistry::GaugeFn fn) {
    if (status) {
      status = registry.addGauge(name, help, std::move(fn));
    }
  }
};

constexpr std::array<const char*, static_cast<std::size_t>(oms::SubmitStatus::kCount_)>
    kSubmitStatusName = {"accepted",  "rejected_by_risk", "rejected_by_venue",
                         "ambiguous", "duplicate",        "blocked_unresolved",
                         "not_ready", "invalid",          "not_durable"};
static_assert(kSubmitStatusName.size() == static_cast<std::size_t>(oms::SubmitStatus::kCount_),
              "every SubmitStatus needs a metric label");

}  // namespace

Result<bool> registerEngineMetrics(MetricsRegistry& registry, const engine::Engine& engine) {
  const auto& s = engine.stats();
  Registrar r{registry};
  r.counter("futu_engine_quotes_received_total", "Quotes offered to the engine.",
            [&s] { return s.received.load(); });
  r.counter("futu_engine_quotes_processed_total", "Quotes the strategy has seen.",
            [&s] { return s.processed.load(); });
  r.counter("futu_engine_quotes_dropped_total",
            "Quotes dropped because the ring was full (strategy was blind).",
            [&s] { return s.dropped.load(); });
  r.counter("futu_engine_quotes_stale_skipped_total",
            "Quotes discarded because they waited too long in the ring.",
            [&s] { return s.staleSkipped.load(); });
  r.counter("futu_engine_quotes_bad_symbol_total", "Quotes refused for an invalid symbol.",
            [&s] { return s.badSymbol.load(); });
  r.counter("futu_engine_strategy_faults_total",
            "Strategy exceptions (each disables the strategy).",
            [&s] { return s.strategyFaults.load(); });
  r.counter("futu_engine_reconciles_total", "Reconciliations run by the reconciler thread.",
            [&s] { return s.reconciles.load(); });
  r.counter("futu_engine_reconcile_problems_total",
            "Reconciliations that were incomplete, found drift, or threw.",
            [&s] { return s.reconcileProblems.load(); });
  r.gauge("futu_engine_ring_high_water", "Deepest quote backlog seen at the start of a poll.",
          [&s] { return static_cast<double>(s.ringHighWater.load()); });
  r.gauge("futu_engine_failed", "1 if an unexpected exception stopped quote handling.",
          [&s] { return s.engineFailed.load() ? 1.0 : 0.0; });
  if (!r.status) {
    return r.status;
  }
  auto queue = registry.addLatencyHistogram(
      "futu_engine_queue_seconds", "Time a quote waited from enqueue to engine pickup.", s.queueNs);
  if (!queue) {
    return queue;
  }
  return registry.addLatencyHistogram("futu_engine_handle_seconds",
                                      "Time the strategy and OMS took per quote.", s.handleNs);
}

Result<bool> registerOmsMetrics(MetricsRegistry& registry, oms::Oms& oms,
                                execution::KillSwitch& kill, execution::RateLimiter& rate) {
  Registrar r{registry};
  for (std::size_t i = 0; i < static_cast<std::size_t>(oms::SubmitStatus::kCount_); ++i) {
    r.counter("futu_oms_submits_total", "Order submits by outcome.",
              [&oms, i] { return oms.stats().submits[i].load(); },
              {{"status", kSubmitStatusName[i]}});
  }
  for (std::size_t i = 1; i <= static_cast<std::size_t>(RiskReject::kSelfTrade); ++i) {
    const auto reason = static_cast<RiskReject>(i);
    r.counter("futu_oms_risk_rejects_total", "Orders refused by pre-trade risk, by reason.",
              [&oms, i] { return oms.stats().riskRejects[i].load(); },
              {{"reason", toString(reason)}});
  }
  r.counter("futu_oms_durable_failures_total",
            "Order intents the write-ahead log refused (each halts trading).",
            [&oms] { return oms.stats().durableFailures.load(); });
  r.counter("futu_oms_restored_intents_total", "Order intents restored from the log at startup.",
            [&oms] { return oms.stats().restoredIntents.load(); });
  r.counter("futu_oms_stale_updates_total",
            "Late 'working' pushes after the order moved on (benign).",
            [&oms] { return oms.stats().staleUpdates.load(); });
  r.counter("futu_oms_anomalies_total", "Illegal transitions and other OMS anomalies.",
            [&oms] { return static_cast<std::uint64_t>(oms.anomalyCount()); });
  r.gauge("futu_oms_live_orders", "Orders currently live at the broker.",
          [&oms] { return static_cast<double>(oms.liveOrderCount()); });
  r.gauge("futu_oms_unresolved_orders", "Orders of unknown outcome awaiting reconciliation.",
          [&oms] { return static_cast<double>(oms.unresolvedCount()); });
  r.gauge("futu_oms_halt_pending", "1 while a cancel-all is scheduled but not finished.",
          [&oms] { return oms.haltPending() ? 1.0 : 0.0; });
  r.gauge("futu_kill_switch_tripped", "1 if the kill switch is tripped (trading blocked).",
          [&kill] { return kill.tripped() ? 1.0 : 0.0; });
  r.gauge("futu_kill_switch_persist_failed",
          "1 if a kill-switch trip could not be written to disk (a restart would not be blocked).",
          [&kill] { return kill.persistFailed() ? 1.0 : 0.0; });
  r.gauge("futu_rate_limit_utilization", "Fraction of the order-rate window budget in use.",
          [&rate] { return rate.utilization(); });
  return r.status;
}

Result<bool> registerWalMetrics(MetricsRegistry& registry, const infra::Wal& wal) {
  const auto& s = wal.stats();
  Registrar r{registry};
  r.counter("futu_wal_records_total", "Records written to the write-ahead log.",
            [&s] { return s.records.load(); });
  r.counter("futu_wal_bytes_total", "Bytes written to the write-ahead log.",
            [&s] { return s.bytes.load(); });
  r.counter("futu_wal_syncs_total", "fdatasync calls.", [&s] { return s.syncs.load(); });
  r.counter("futu_wal_dropped_async_total", "Audit records dropped because the queue was full.",
            [&s] { return s.droppedAsync.load(); });
  r.counter("futu_wal_write_errors_total", "Failed WAL writes (the log is no longer complete).",
            [&s] { return s.writeErrors.load(); });
  r.gauge("futu_wal_failed", "1 once a WAL write has failed.",
          [&wal] { return wal.failed() ? 1.0 : 0.0; });
  if (!r.status) {
    return r.status;
  }
  return registry.addLatencyHistogram("futu_wal_sync_seconds", "fdatasync latency.", s.syncNs);
}

}  // namespace futu_trader::app
