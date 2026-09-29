#pragma once

#include <cstdint>
#include <string>

#include "futu_trader/oms/live_gate.hpp"

namespace futu_trader::testing_support {

// A well-formed live-gate input for `accId` with five clean SIMULATE days ending today.
inline oms::LiveGateInput validLiveInput(std::uint64_t accId,
                                         const std::string& today = "2026-09-06") {
  oms::LiveGateInput input;
  input.configuredAccId = accId;
  input.configPhrase = oms::LiveGate::expectedPhrase(accId);
  input.envVar = oms::LiveGate::kEnvValue;
  input.cliLiveFlag = true;
  input.tradeUnlocked = true;
  input.startupReconcileClean = true;
  input.today = today;
  for (int day = 1; day <= 5; ++day) {
    input.promotion.push_back({"2026-09-0" + std::to_string(day), true});
  }
  opend::TrdAccount real;
  real.env = TrdEnv::kReal;
  real.accId = accId;
  input.brokerAccounts = {real};
  return input;
}

// The only legitimate way to a REAL target is through the gate; tests do the same.
inline oms::TradeTarget makeRealTarget(std::uint64_t accId) {
  const auto approval = oms::LiveGate::approveReal(validLiveInput(accId));
  return oms::TradeTarget::real(approval.value(), accId, opend::TrdMarket::kHK).value();
}

}  // namespace futu_trader::testing_support
