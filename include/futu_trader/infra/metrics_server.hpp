#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "futu_trader/core/result.hpp"
#include "futu_trader/infra/metrics.hpp"

namespace futu_trader::infra {

struct MetricsServerConfig {
  std::uint16_t port{0};  // 0 = pick an ephemeral port (start() reports it)
  /** A client that has not finished sending its request headers by then is dropped. */
  std::chrono::milliseconds readTimeout{2000};
  std::size_t maxRequestBytes{8192};
};

/**
 * A tiny read-only HTTP/1.1 endpoint for operators and Prometheus:
 *   GET /metrics   the exposition from the registry
 *   GET /healthz   200 while the process is responsive (liveness)
 *   GET /readyz    200 only if `ready()` says we are fit to trade, else 503 (readiness)
 *
 * Deliberately minimal and defensive: it binds to 127.0.0.1 only (put a reverse proxy or the
 * Prometheus agent next to it; this process must not expose an unauthenticated port), reads at
 * most `maxRequestBytes`, enforces a read deadline (no slow-loris), answers only GET, closes after
 * every response, and exposes nothing but the registry. It cannot change any state.
 */
class MetricsServer {
 public:
  using ReadyFn = std::function<bool()>;

  MetricsServer(const MetricsRegistry& registry, ReadyFn ready, MetricsServerConfig config);
  ~MetricsServer();
  MetricsServer(const MetricsServer&) = delete;
  MetricsServer& operator=(const MetricsServer&) = delete;

  /** Starts listening on loopback; returns the bound port. */
  Result<std::uint16_t> start();
  void stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace futu_trader::infra
