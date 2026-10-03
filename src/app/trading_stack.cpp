#include "futu_trader/app/trading_stack.hpp"

#include <cassert>
#include <set>

#include "futu_trader/app/application.hpp"
#include "futu_trader/app/config_mapping.hpp"
#include "futu_trader/app/metrics_bindings.hpp"
#include "futu_trader/app/wall_time.hpp"
#include "futu_trader/opend/proto_ids.hpp"
#include "futu_trader/strategy/strategy_factory.hpp"

namespace futu_trader::app {
namespace {

bool isTradePush(std::uint32_t id) {
  return id == opend::protoId::kTrdUpdateOrder || id == opend::protoId::kTrdUpdateOrderFill;
}

std::set<std::string> subscribedCodes(const AppConfig& cfg) {
  std::set<std::string> codes;
  for (const auto& sym : cfg.symbols) {
    codes.insert(sym.code);
  }
  return codes;
}

}  // namespace

TradingStack::TradingStack(const AppConfig& cfg, const std::filesystem::path& stateDir,
                           opend::OpenDClient& client, LinkGuard& guard,
                           const oms::TradeTarget& target, execution::KillSwitch& kill,
                           infra::Wal& wal, const std::vector<oms::DurableSubmit>& pastIntents,
                           Log log)
    : cfg_(cfg),
      client_(client),
      guard_(guard),
      kill_(kill),
      wal_(wal),
      log_(std::move(log)),
      epoch_(std::to_string(wallNowNs() / 1'000'000)),
      instruments_(makeInstruments(cfg)),
      rate_(rateConfig(cfg), clock_),
      risk_(riskConfig(cfg), preTradeConfig(cfg), kill, instruments_),
      venue_(client, target),
      oms_(venue_, risk_, rate_, kill, book_, clock_,
           [&] {
             auto oc = omsConfig(cfg, epoch_);
             oc.requireDurable = true;  // never send an order without the write-ahead sink
             return oc;
           }()),
      router_(oms_, target),
      strategy_(strategy::makeStrategy(strategySpec(cfg))),
      engine_(oms_, *strategy_, book_, clock_, kill, engineConfig(cfg, epoch_, stateDir)),
      quotes_(subscribedCodes(cfg)) {
  assert(strategy_ != nullptr);  // the config validator only admits known strategy names

  // Write-ahead: the intent is durable before the order can leave the process. The record carries
  // WALL-clock time (the OMS clock is monotonic and means nothing after a restart).
  oms_.setDurableSubmitSink([&wal](const oms::DurableSubmit& d) {
    oms::DurableSubmit stamped = d;
    stamped.tsNs = wallNowNs();
    return wal.appendDurable(oms::encodeSubmit(stamped));
  });
  oms_.setJournalSink([&wal](const oms::JournalEntry& e) {
    oms::JournalEntry stamped = e;
    stamped.tsNs = wallNowNs();
    wal.appendAsync(oms::encodeJournal(stamped));
  });
  if (const auto restored = oms_.restoreIntents(pastIntents, startOfHkDayNs(wallNowNs()));
      restored > 0) {
    log_("restored " + std::to_string(restored) +
         " order intent(s) from the log; they stay blocked until reconciled");
  }

  client_.setPushHandler([this](const opend::Frame& frame) { onPush(frame); });
  // Losing OpenD means we cannot see fills or cancel: halt. (The persistent kill switch then needs
  // a human; the process cancels what it can and exits.)
  client_.setConnectionStateHandler([this](bool connected) {
    if (!connected) {
      oms_.requestHalt("OpenD connection lost");
    }
  });
}

TradingStack::~TradingStack() { guard_.release(); }  // joins the reader BEFORE members go away

// Runs on the OpenD reader thread, the engine's single producer. It never calls the venue.
void TradingStack::onPush(const opend::Frame& frame) {
  if (isTradePush(frame.protoId)) {
    router_.onFrame(frame);
    return;
  }
  const auto quote = quotes_.onFrame(frame, clock_.nowNs());
  if (!quote) {
    quoteDecodeErrors_.fetch_add(1);
  } else if (const auto& event = quote.value(); event.has_value()) {
    engine_.onQuote(*event);
  }
}

std::optional<int> TradingStack::prepare() {
  std::vector<opend::SecurityRef> securities;
  securities.reserve(cfg_.symbols.size());
  for (const auto& sym : cfg_.symbols) {
    securities.push_back({opend::kQotMarketHkSecurity, sym.code});
  }
  if (const auto sub = client_.subscribeAccountPush({cfg_.account.id}); !sub) {
    log_("cannot subscribe to account pushes: " + sub.error().message);
    return kExitConnect;
  }
  if (const auto sub =
          client_.subscribe(securities, {opend::SubType::kBasic, opend::SubType::kOrderBook});
      !sub) {
    log_("cannot subscribe to market data: " + sub.error().message);
    return kExitConnect;
  }

  // Learn the account's true state, and refuse to trade unless it is understood.
  if (const auto boot = oms_.bootstrap(); !boot) {
    log_("cannot bootstrap from the broker: " + boot.error().message);
    return kExitConnect;
  }
  const auto first = oms_.reconcile();
  if (!first.complete) {
    log_("refused: first reconciliation incomplete: " + first.error);
    return kExitConnect;
  }
  if (!first.drifts.empty() || kill_.tripped()) {
    log_("refused: first reconciliation found " + std::to_string(first.drifts.size()) +
         " unexplained difference(s); the kill switch is now tripped");
    for (const auto& d : first.drifts) {
      log_("  drift: " + d.detail);
    }
    oms_.haltAndCancelAll("startup reconciliation found drift");
    return kExitRefused;
  }
  return std::nullopt;
}

bool TradingStack::registerMetrics(infra::MetricsRegistry& registry) {
  using infra::MetricsRegistry;
  bool ok = registerOmsMetrics(registry, oms_, kill_, rate_).ok() &&
            registerEngineMetrics(registry, engine_).ok() &&
            registerWalMetrics(registry, wal_).ok();
  const auto counter = [&](const char* name, const char* help, MetricsRegistry::CounterFn fn) {
    ok = ok && registry.addCounter(name, help, std::move(fn)).ok();
  };
  counter("futu_push_foreign_total", "Pushes for another account/env/market.",
          [this] { return static_cast<std::uint64_t>(router_.foreign()); });
  counter("futu_push_undecodable_total", "Trade pushes that could not be decoded.",
          [this] { return static_cast<std::uint64_t>(router_.undecodable()); });
  counter("futu_quote_decode_errors_total", "Malformed quote pushes.",
          [this] { return quoteDecodeErrors_.load(); });
  counter("futu_opend_reconnects_total", "OpenD reconnections.",
          [this] { return static_cast<std::uint64_t>(client_.reconnectCount()); });
  ok = ok && registry
                 .addGauge("futu_opend_connected", "1 while the OpenD link is up.",
                           [this] { return client_.isConnected() ? 1.0 : 0.0; })
                 .ok();
  return ok;
}

bool TradingStack::ready() const {
  return client_.isConnected() && !kill_.tripped() && !oms_.haltPending() &&
         !engine_.stats().engineFailed.load() && !wal_.failed();
}

}  // namespace futu_trader::app
