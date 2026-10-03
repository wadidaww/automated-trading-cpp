#include "futu_trader/oms/live_gate.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>

namespace futu_trader::oms {

namespace {

// Days since 1970-01-01 for a valid proleptic-Gregorian date, or nullopt for anything that is not
// a real calendar date (month 13, Feb 30, 2026-02-29, ...).
std::optional<std::int64_t> daysFromDate(const std::string& text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9')) {
      return std::nullopt;
    }
  }
  const int year = std::stoi(text.substr(0, 4));
  const int month = std::stoi(text.substr(5, 2));
  const int day = std::stoi(text.substr(8, 2));
  static constexpr std::array<int, 12> kDaysInMonth{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (year < 1970 || month < 1 || month > 12 || day < 1) {
    return std::nullopt;
  }
  if (day > kDaysInMonth.at(static_cast<std::size_t>(month - 1)) + ((month == 2 && leap) ? 1 : 0)) {
    return std::nullopt;
  }
  // Howard Hinnant's days_from_civil.
  const int y = month <= 2 ? year - 1 : year;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - (era * 400);
  const int doy = ((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5) + day - 1;
  const int doe = (yoe * 365) + (yoe / 4) - (yoe / 100) + doy;
  return static_cast<std::int64_t>(era) * 146097 + doe - 719468;
}

}  // namespace

Result<std::vector<PromotionEntry>> parsePromotionLog(const std::string& text) {
  if (text.size() > kMaxPromotionLogBytes) {
    return Error{ErrorCode::kInvalidArg, "promotion log too large: treated as corrupt"};
  }
  std::vector<PromotionEntry> out;
  std::istringstream lines(text);
  std::string line;
  int lineNo = 0;
  while (std::getline(lines, line)) {
    ++lineNo;
    if (line.empty()) {
      continue;
    }
    std::istringstream fields(line);
    std::string date;
    std::string verdict;
    std::string extra;
    if (!(fields >> date >> verdict) || (fields >> extra) || !daysFromDate(date) ||
        (verdict != "clean" && verdict != "dirty")) {
      return Error{ErrorCode::kInvalidArg,
                   "promotion log line " + std::to_string(lineNo) + " is malformed"};
    }
    out.push_back({date, verdict == "clean"});
  }
  return out;
}

std::string formatPromotionEntry(const PromotionEntry& entry) {
  return entry.date + (entry.clean ? " clean\n" : " dirty\n");
}

bool promotionEligible(const std::vector<PromotionEntry>& log, std::size_t requiredDays,
                       const std::string& today, int maxGapDays, int maxStaleDays) {
  const auto todayDays = daysFromDate(today);
  if (requiredDays == 0 || log.size() < requiredDays || !todayDays) {
    return false;
  }
  const auto first = log.end() - static_cast<std::ptrdiff_t>(requiredDays);
  std::optional<std::int64_t> previous;
  for (auto it = first; it != log.end(); ++it) {
    const auto day = daysFromDate(it->date);
    if (!it->clean || !day) {
      return false;
    }
    if (previous && (*day <= *previous || *day - *previous > maxGapDays)) {
      return false;  // duplicate, out of order, or a gap too large to be consecutive sessions
    }
    previous = day;
  }
  if (!previous) {
    return false;
  }
  return *previous <= *todayDays && *todayDays - *previous <= maxStaleDays;
}

std::string LiveGate::expectedPhrase(std::uint64_t accId) {
  std::string digits = std::to_string(accId);
  if (digits.size() > 4) {
    digits = digits.substr(digits.size() - 4);
  }
  return "I-ACCEPT-REAL-MONEY-" + digits;
}

namespace {

// Every REAL gate except the startup reconciliation. Returns the refusal, or empty when all pass.
std::string firstRefusalExceptReconcile(const LiveGateInput& in) {
  if (in.configuredAccId == 0) {
    return "no account id configured";
  }
  if (in.configPhrase != LiveGate::expectedPhrase(in.configuredAccId)) {
    return "config live_ack phrase missing or wrong";
  }
  if (in.envVar != LiveGate::kEnvValue) {
    return "FUTU_LIVE_TRADING environment variable not set to the required value";
  }
  if (!in.cliLiveFlag) {
    return "--live flag not given";
  }
  if (!promotionEligible(in.promotion, in.requiredCleanDays, in.today, in.maxGapDays,
                         in.maxStaleDays)) {
    return "promotion record does not show " + std::to_string(in.requiredCleanDays) +
           " recent consecutive clean SIMULATE days";
  }
  if (!in.tradeUnlocked) {
    return "trading has not been unlocked this session";
  }
  const bool accountKnown = std::any_of(
      in.brokerAccounts.begin(), in.brokerAccounts.end(),
      [&](const auto& acc) { return acc.accId == in.configuredAccId && acc.env == TrdEnv::kReal; });
  if (!accountKnown) {
    return "configured account is not a REAL account reported by the broker";
  }
  return {};
}

Error refuseReal(const std::string& why) {
  return Error{ErrorCode::kInvalidArg, "REAL trading refused: " + why};
}

}  // namespace

Result<PendingLiveApproval> LiveGate::approveRealPending(const LiveGateInput& in) {
  const auto why = firstRefusalExceptReconcile(in);
  if (!why.empty()) {
    return refuseReal(why);
  }
  return PendingLiveApproval(in.configuredAccId);
}

Result<LiveApproval> LiveGate::confirm(const PendingLiveApproval& pending, bool reconcileClean) {
  if (!reconcileClean) {
    return refuseReal("startup reconciliation has not completed cleanly");
  }
  return LiveApproval(pending.accId());
}

Result<LiveApproval> LiveGate::approveReal(const LiveGateInput& in) {
  const auto pending = approveRealPending(in);
  if (!pending) {
    return pending.error();
  }
  return confirm(pending.value(), in.startupReconcileClean);
}

Result<TradeTarget> TradeTarget::realReadOnly(const PendingLiveApproval& pending,
                                              std::uint64_t accId, opend::TrdMarket market) {
  if (pending.accId() != accId) {
    return Error{ErrorCode::kInvalidArg, "live approval was issued for a different account"};
  }
  return TradeTarget(TrdEnv::kReal, accId, market, /*readOnly=*/true);
}

Result<TradeTarget> TradeTarget::real(const LiveApproval& approval, std::uint64_t accId,
                                      opend::TrdMarket market) {
  if (approval.accId() != accId) {
    return Error{ErrorCode::kInvalidArg, "live approval was issued for a different account"};
  }
  return TradeTarget(TrdEnv::kReal, accId, market);
}

}  // namespace futu_trader::oms
