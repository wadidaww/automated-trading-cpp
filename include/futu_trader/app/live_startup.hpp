#pragma once

#include <vector>

#include "futu_trader/app/application.hpp"
#include "futu_trader/app/config.hpp"
#include "futu_trader/app/link_guard.hpp"
#include "futu_trader/app/log.hpp"
#include "futu_trader/core/result.hpp"
#include "futu_trader/oms/live_gate.hpp"
#include "futu_trader/opend/client.hpp"

namespace futu_trader::app {

/**
 * The account the config names must exist at the broker, be of the environment the config's mode
 * asks for (a SIMULATE config pointed at a REAL account, or the reverse, is refused) and have Hong
 * Kong trading authorisation.
 */
Result<bool> verifyAccount(const AppConfig& cfg, const std::vector<opend::TrdAccount>& accounts);

/**
 * Decides where orders will go. SIMULATE needs nothing. REAL runs the whole live gate:
 *   1. every condition that does not need the unlock (phrase, env var, --live, promotion record,
 *      account) is checked BEFORE anything is unlocked, so a refusal never leaves the real account
 *      unlocked;
 *   2. trading is unlocked (the guard re-locks on every way out);
 *   3. a READ-ONLY startup reconciliation of the real account must come back clean;
 *   4. only then does the gate issue the approval a REAL target requires.
 * The error message says which step refused.
 */
Result<oms::TradeTarget> selectTarget(const AppConfig& cfg, const RunOptions& opts,
                                      opend::OpenDClient& client, LinkGuard& guard,
                                      const std::vector<opend::TrdAccount>& accounts,
                                      const Log& log);

}  // namespace futu_trader::app
