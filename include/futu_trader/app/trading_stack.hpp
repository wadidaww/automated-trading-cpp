#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "futu_trader/app/config.hpp"
#include "futu_trader/app/link_guard.hpp"
#include "futu_trader/app/log.hpp"
#include "futu_trader/core/clock.hpp"
#include "futu_trader/engine/engine.hpp"
#include "futu_trader/execution/kill_switch.hpp"
#include "futu_trader/execution/rate_limiter.hpp"
#include "futu_trader/infra/metrics.hpp"
#include "futu_trader/infra/wal.hpp"
#include "futu_trader/instrument/hk_rules.hpp"
#include "futu_trader/oms/journal_codec.hpp"
#include "futu_trader/oms/live_gate.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/oms/opend_venue.hpp"
#include "futu_trader/oms/push_router.hpp"
#include "futu_trader/opend/client.hpp"
#include "futu_trader/opend/quote_decode.hpp"
#include "futu_trader/portfolio/position_book.hpp"
#include "futu_trader/strategy/strategy.hpp"

namespace futu_trader::app {

/**
 * The composition root of a trading process: builds the venue, risk, OMS, router, strategy, engine
 * and quote decoder from the config, wires the write-ahead sinks and the OpenD push handlers, and
 * owns their lifetime in dependency order.
 *
 * Lifetime rule that matters: the OpenD reader thread calls into everything owned here. The
 * destructor therefore releases the link (re-lock, close, JOIN the reader) before any member is
 * destroyed, so no push can reach a half-destroyed object on any exit path.
 */
class TradingStack {  // NOLINT(clang-analyzer-optin.performance.Padding): owns cache-line aligned
                      // Engine
 public:
  TradingStack(const AppConfig& cfg, const std::filesystem::path& stateDir,
               opend::OpenDClient& client, LinkGuard& guard, const oms::TradeTarget& target,
               execution::KillSwitch& kill, infra::Wal& wal,
               const std::vector<oms::DurableSubmit>& pastIntents, Log log);
  ~TradingStack();
  TradingStack(const TradingStack&) = delete;
  TradingStack& operator=(const TradingStack&) = delete;

  /**
   * Subscribes to account and market data, learns the account's true state from the broker and
   * reconciles once. Returns an exit code if trading must not start, nullopt if it may.
   */
  std::optional<int> prepare();

  /** Registers every metric of the process. False if any registration was refused. */
  bool registerMetrics(infra::MetricsRegistry& registry);
  /** Fit to trade right now (the /readyz answer). */
  bool ready() const;

  oms::Oms& oms() { return oms_; }
  engine::Engine& engine() { return engine_; }
  const oms::PushRouter& router() const { return router_; }

 private:
  void onPush(const opend::Frame& frame);

  const AppConfig& cfg_;
  opend::OpenDClient& client_;
  LinkGuard& guard_;
  execution::KillSwitch& kill_;
  infra::Wal& wal_;
  Log log_;

  const std::string epoch_;  // unique per process start: part of every ClOrdId and intent key
  SteadyClock clock_;
  instrument::InstrumentTable instruments_;
  portfolio::PositionBook book_;
  execution::RateLimiter rate_;
  oms::PreTradeRisk risk_;
  oms::OpenDVenue venue_;
  oms::Oms oms_;
  oms::PushRouter router_;
  std::unique_ptr<strategy::IStrategy> strategy_;
  engine::Engine engine_;
  opend::QuoteAssembler quotes_;  // touched only by the single OpenD reader thread
  std::atomic<std::uint64_t> quoteDecodeErrors_{0};
};

}  // namespace futu_trader::app
