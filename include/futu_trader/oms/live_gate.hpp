#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/core/types.hpp"
#include "futu_trader/opend/types.hpp"

namespace futu_trader::oms {

/** One day's reconciliation outcome in the promotion log. */
struct PromotionEntry {
  std::string date;  // YYYY-MM-DD
  bool clean{false};
};

/** Longest promotion log we will parse; a bigger file is treated as corrupt, not truncated. */
inline constexpr std::size_t kMaxPromotionLogBytes = std::size_t{256} * 1024;

/**
 * Parses lines of "YYYY-MM-DD clean|dirty". Malformed lines, impossible calendar dates or an
 * oversized file make the whole log invalid; nothing is partially trusted.
 */
Result<std::vector<PromotionEntry>> parsePromotionLog(const std::string& text);
std::string formatPromotionEntry(const PromotionEntry& entry);

/**
 * True only if the most recent `requiredDays` entries are all clean, on real calendar dates in
 * strictly increasing order with no gap larger than `maxGapDays` (covers weekends and holiday
 * closures without a trading calendar), and the newest entry is not in the future and is at most
 * `maxStaleDays` before `today`. So neither a stale log nor invented dates can promote.
 * A plain text file has no integrity protection: this is a guard against mistakes and stale
 * state, not against a local attacker who can write the file.
 */
bool promotionEligible(const std::vector<PromotionEntry>& log, std::size_t requiredDays,
                       const std::string& today, int maxGapDays = 7, int maxStaleDays = 5);

struct LiveGateInput {
  std::string configPhrase;  // trading.live_ack from config
  std::string envVar;        // value of FUTU_LIVE_TRADING
  bool cliLiveFlag{false};   // --live
  std::vector<PromotionEntry> promotion;
  std::size_t requiredCleanDays{5};
  std::string today;  // YYYY-MM-DD from the trading calendar/clock, never from the log itself
  int maxGapDays{7};
  int maxStaleDays{5};
  bool tradeUnlocked{false};          // Trd_UnlockTrade succeeded this session
  bool startupReconcileClean{false};  // funds/positions/orders reconciled before the first order
  std::uint64_t configuredAccId{0};
  std::vector<opend::TrdAccount> brokerAccounts;  // from Trd_GetAccList
};

/**
 * Proof that every REAL-trading gate passed. It can only be created by LiveGate::approveReal, and
 * is bound to one account id, so a REAL TradeTarget cannot exist without going through the gate.
 */
class LiveApproval {
 public:
  std::uint64_t accId() const { return accId_; }

 private:
  friend class LiveGate;
  explicit LiveApproval(std::uint64_t accId) : accId_(accId) {}
  std::uint64_t accId_;
};

class LiveGate {
 public:
  static constexpr const char* kEnvValue = "I_UNDERSTAND_REAL_MONEY";
  /** Phrase the operator must put in config; includes the last 4 digits of the account id. */
  static std::string expectedPhrase(std::uint64_t accId);

  /** Every condition must hold; the error names the first one that does not. */
  static Result<LiveApproval> approveReal(const LiveGateInput& input);
};

/** Which account and environment orders are sent to. REAL requires a LiveApproval. */
class TradeTarget {
 public:
  static TradeTarget simulate(std::uint64_t accId, opend::TrdMarket market) {
    return {TrdEnv::kSimulate, accId, market};
  }
  /** Fails if the approval was issued for a different account. */
  static Result<TradeTarget> real(const LiveApproval& approval, std::uint64_t accId,
                                  opend::TrdMarket market);

  TrdEnv env() const { return env_; }
  std::uint64_t accId() const { return accId_; }
  opend::TrdMarket market() const { return market_; }
  opend::AccountHeader header() const { return {env_, accId_, market_}; }

 private:
  TradeTarget(TrdEnv env, std::uint64_t accId, opend::TrdMarket market)
      : env_(env), accId_(accId), market_(market) {}
  TrdEnv env_;
  std::uint64_t accId_;
  opend::TrdMarket market_;
};

}  // namespace futu_trader::oms
