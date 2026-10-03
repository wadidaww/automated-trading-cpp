#include "futu_trader/app/live_startup.hpp"

#include <algorithm>

#include "futu_trader/app/config_mapping.hpp"
#include "futu_trader/app/trusted_fs.hpp"
#include "futu_trader/app/wall_time.hpp"
#include "futu_trader/core/clock.hpp"
#include "futu_trader/execution/kill_switch.hpp"
#include "futu_trader/execution/rate_limiter.hpp"
#include "futu_trader/infra/secrets.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/oms/opend_venue.hpp"
#include "futu_trader/portfolio/position_book.hpp"

namespace futu_trader::app {
namespace {

Error refuse(const std::string& why) { return Error{ErrorCode::kInvalidArg, why}; }

// A throwaway OMS over a READ-ONLY view of the real account: bootstrap and reconcile once, to prove
// the account is in a state we understand BEFORE the live gate may approve. It cannot place orders
// (the read-only target is refused at the wire layer) and its state is discarded.
bool startupReconcileIsClean(const AppConfig& cfg, opend::OpenDClient& client,
                             const oms::PendingLiveApproval& pending, const Log& log) {
  const auto target =
      oms::TradeTarget::realReadOnly(pending, cfg.account.id, opend::TrdMarket::kHK);
  if (!target) {
    log("startup check: " + target.error().message);
    return false;
  }
  oms::OpenDVenue venue(client, target.value());
  SteadyClock clock;
  execution::KillSwitch kill;
  execution::RateLimiter rate(rateConfig(cfg), clock);
  auto instruments = makeInstruments(cfg);
  oms::PreTradeRisk risk(riskConfig(cfg), preTradeConfig(cfg), kill, instruments);
  portfolio::PositionBook book;
  oms::OmsConfig oc = omsConfig(cfg, "STARTUPCHECK");
  oc.reserveOrders = 1000;
  oms::Oms oms(venue, risk, rate, kill, book, clock, oc);
  if (const auto boot = oms.bootstrap(); !boot) {
    log("startup check: bootstrap failed: " + boot.error().message);
    return false;
  }
  for (const auto& order : oms.orders()) {
    if (order.external && oms::isLive(order.state)) {
      // We would adopt a stranger's live order and could cancel it in a halt: not something to
      // discover with real money at stake.
      log("startup check: the account has a live order we did not place (" + order.symbol +
          "); cancel it or trade another account");
      return false;
    }
  }
  const auto report = oms.reconcile();
  if (!report.clean() || oms.unresolvedCount() != 0 || kill.tripped()) {
    log("startup check: reconciliation not clean: " +
        (report.error.empty() ? std::to_string(report.drifts.size()) + " drift(s)" : report.error));
    return false;
  }
  return true;
}

Result<oms::TradeTarget> approveRealTarget(const AppConfig& cfg, const RunOptions& opts,
                                           opend::OpenDClient& client, LinkGuard& guard,
                                           const std::vector<opend::TrdAccount>& accounts,
                                           const Log& log) {
  auto secret = infra::readSecretFile(cfg.tradePasswordMd5File);
  if (!secret) {
    return secret.error();
  }
  auto promotion = readTrustedText(cfg.live.promotionLog, oms::kMaxPromotionLogBytes);
  if (!promotion) {
    return refuse("promotion log: " + promotion.error().message);
  }
  auto entries = oms::parsePromotionLog(promotion.value());
  if (!entries) {
    return refuse("promotion log is invalid: " + entries.error().message);
  }
  oms::LiveGateInput gate;
  gate.configPhrase = cfg.live.ackPhrase;
  gate.envVar = opts.liveEnv;
  gate.cliLiveFlag = opts.live;
  gate.promotion = std::move(entries.value());
  gate.requiredCleanDays = cfg.live.requiredCleanDays;
  gate.today = opts.today.empty() ? hkDate(wallNowNs()) : opts.today;
  gate.configuredAccId = cfg.account.id;
  gate.brokerAccounts = accounts;

  // Dry run with the unlock assumed: only the unlock itself is still outstanding.
  oms::LiveGateInput dryRun = gate;
  dryRun.tradeUnlocked = true;
  if (const auto early = oms::LiveGate::approveRealPending(dryRun); !early) {
    return early.error();
  }
  if (const auto unlocked = client.unlockTrade(secret.value()); !unlocked) {
    return refuse("trade unlock failed: " + unlocked.error().message);
  }
  guard.markUnlocked();
  gate.tradeUnlocked = true;
  const auto pending = oms::LiveGate::approveRealPending(gate);
  if (!pending) {
    return pending.error();
  }
  const bool clean = startupReconcileIsClean(cfg, client, pending.value(), log);
  const auto approval = oms::LiveGate::confirm(pending.value(), clean);
  if (!approval) {
    return approval.error();
  }
  return oms::TradeTarget::real(approval.value(), cfg.account.id, opend::TrdMarket::kHK);
}

}  // namespace

Result<bool> verifyAccount(const AppConfig& cfg, const std::vector<opend::TrdAccount>& accounts) {
  const bool real = cfg.mode == Mode::kReal;
  const auto account = std::find_if(accounts.begin(), accounts.end(),
                                    [&](const auto& a) { return a.accId == cfg.account.id; });
  if (account == accounts.end()) {
    return refuse("account " + std::to_string(cfg.account.id) + " is not reported by OpenD");
  }
  if (account->env != (real ? TrdEnv::kReal : TrdEnv::kSimulate)) {
    return refuse("account " + std::to_string(cfg.account.id) + " is a " +
                  (account->env == TrdEnv::kReal ? "REAL" : "SIMULATE") +
                  " account but config mode is '" + (real ? "real" : "simulate") + "'");
  }
  const auto hk = static_cast<std::int32_t>(opend::TrdMarket::kHK);
  if (std::find(account->markets.begin(), account->markets.end(), hk) == account->markets.end()) {
    return refuse("the account has no Hong Kong trading authorisation");
  }
  return true;
}

Result<oms::TradeTarget> selectTarget(const AppConfig& cfg, const RunOptions& opts,
                                      opend::OpenDClient& client, LinkGuard& guard,
                                      const std::vector<opend::TrdAccount>& accounts,
                                      const Log& log) {
  if (cfg.mode == Mode::kSimulate) {
    return oms::TradeTarget::simulate(cfg.account.id, opend::TrdMarket::kHK);
  }
  auto target = approveRealTarget(cfg, opts, client, guard, accounts, log);
  if (target) {
    log("live gate passed for account " + std::to_string(cfg.account.id));
  }
  return target;
}

}  // namespace futu_trader::app
