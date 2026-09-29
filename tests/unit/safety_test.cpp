#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "futu_trader/core/clock.hpp"
#include "futu_trader/execution/kill_switch.hpp"
#include "futu_trader/execution/rate_limiter.hpp"
#include "futu_trader/oms/live_gate.hpp"
#include "test_support.hpp"

using namespace futu_trader;
using namespace futu_trader::execution;
using namespace futu_trader::oms;

// --- Rate limiter -------------------------------------------------------------------------------

TEST(RateLimiter, NewOrdersStopBeforeTheCancelReserve) {
  ManualClock clock;
  RateLimiter limiter({.maxPerWindow = 5, .windowMs = 30'000, .reservedForCancels = 2}, clock);
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(limiter.tryAcquire(RequestKind::kNewOrder)) << i;
  }
  EXPECT_FALSE(limiter.tryAcquire(RequestKind::kNewOrder));  // remaining 2 are for cancels
  EXPECT_FALSE(limiter.tryAcquire(RequestKind::kModify));
  EXPECT_TRUE(limiter.tryAcquire(RequestKind::kCancel));
  EXPECT_TRUE(limiter.tryAcquire(RequestKind::kCancel));
  EXPECT_FALSE(limiter.tryAcquire(RequestKind::kCancel));  // hard cap reached
  EXPECT_EQ(limiter.used(), 5U);
}

TEST(RateLimiter, WindowSlidesWithTime) {
  ManualClock clock;
  RateLimiter limiter({.maxPerWindow = 2, .windowMs = 1000, .reservedForCancels = 0}, clock);
  EXPECT_TRUE(limiter.tryAcquire(RequestKind::kNewOrder));
  clock.advanceMs(400);
  EXPECT_TRUE(limiter.tryAcquire(RequestKind::kNewOrder));
  EXPECT_FALSE(limiter.tryAcquire(RequestKind::kNewOrder));
  clock.advanceMs(599);  // first stamp is 999 ms old: still inside the window
  EXPECT_FALSE(limiter.tryAcquire(RequestKind::kNewOrder));
  clock.advanceMs(1);  // exactly 1000 ms: it expires
  EXPECT_TRUE(limiter.tryAcquire(RequestKind::kNewOrder));
}

TEST(RateLimiter, BurstNeverExceedsBudgetInAnyWindow) {
  // Property: however requests are spaced, no 30 s window ever holds more than the cap.
  ManualClock clock;
  const RateLimitConfig cfg{.maxPerWindow = 12, .windowMs = 30'000, .reservedForCancels = 3};
  RateLimiter limiter(cfg, clock);
  std::vector<std::int64_t> granted;
  for (int i = 0; i < 2000; ++i) {
    clock.advanceMs((i * 37) % 900);  // irregular spacing
    if (limiter.tryAcquire(RequestKind::kNewOrder)) {
      granted.push_back(clock.nowNs());
    }
  }
  for (std::size_t i = 0; i < granted.size(); ++i) {
    std::size_t inWindow = 0;
    for (std::size_t j = i; j < granted.size() && granted[j] - granted[i] < 30'000LL * 1'000'000;
         ++j) {
      ++inWindow;
    }
    ASSERT_LE(inWindow, cfg.maxPerWindow - cfg.reservedForCancels);
  }
}

TEST(RateLimiter, UtilizationReportsFraction) {
  ManualClock clock;
  RateLimiter limiter({.maxPerWindow = 10, .windowMs = 1000, .reservedForCancels = 0}, clock);
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(limiter.tryAcquire(RequestKind::kNewOrder));
  }
  EXPECT_DOUBLE_EQ(limiter.utilization(), 0.8);
}

// --- Kill switch --------------------------------------------------------------------------------

TEST(KillSwitch, StaysTrippedUntilHumanReset) {
  KillSwitch ks;
  EXPECT_FALSE(ks.tripped());
  ks.trip("daily loss");
  ks.trip("later consequence");
  EXPECT_TRUE(ks.tripped());
  EXPECT_EQ(ks.reason(), "daily loss");  // first cause is preserved
  EXPECT_TRUE(ks.reset("alice"));
  EXPECT_FALSE(ks.tripped());
  EXPECT_EQ(ks.lastResetBy(), "alice");
}

TEST(KillSwitch, ResetWithoutAnOperatorNameIsRefused) {
  KillSwitch ks;
  ks.trip("x");
  EXPECT_FALSE(ks.reset(""));
  EXPECT_TRUE(ks.tripped());
}

TEST(KillSwitch, TripSurvivesARestartUntilAHumanResets) {
  const auto path = std::filesystem::temp_directory_path() / "futu_kill_persist_test";
  std::filesystem::remove(path);
  {
    KillSwitch first(path.string());
    EXPECT_FALSE(first.tripped());
    first.trip("reconciliation drift: 00700 ours 100 broker 300");
  }
  KillSwitch afterRestart(path.string());  // a new process
  EXPECT_TRUE(afterRestart.tripped());     // bouncing the process must not clear a halt
  EXPECT_NE(afterRestart.reason().find("reconciliation drift"), std::string::npos);
  EXPECT_TRUE(afterRestart.reset("alice"));
  EXPECT_FALSE(afterRestart.tripped());
  KillSwitch afterReset(path.string());
  EXPECT_FALSE(afterReset.tripped());  // the reset removed the persisted trip
  std::filesystem::remove(path);
}

TEST(KillSwitch, FlagFileTripsIt) {
  const auto path = std::filesystem::temp_directory_path() / "futu_kill_flag_test";
  std::filesystem::remove(path);
  KillSwitch ks;
  EXPECT_FALSE(ks.checkFlagFile(path.string()));
  EXPECT_FALSE(ks.tripped());
  std::ofstream(path).put('x');
  EXPECT_TRUE(ks.checkFlagFile(path.string()));
  EXPECT_TRUE(ks.tripped());
  std::filesystem::remove(path);
}

TEST(KillSwitch, DanglingSymlinkFlagCountsAsPresent) {
  const auto path = std::filesystem::temp_directory_path() / "futu_kill_flag_dangling";
  std::filesystem::remove(path);
  std::filesystem::create_symlink("/nonexistent/futu/target", path);
  KillSwitch ks;
  EXPECT_TRUE(ks.checkFlagFile(path.string()));  // exists() would have said "no flag" here
  EXPECT_TRUE(ks.tripped());
  std::filesystem::remove(path);
}

TEST(KillSwitch, UnexaminableFlagPathFailsClosed) {
  if (geteuid() == 0) {
    GTEST_SKIP() << "root ignores directory permissions";
  }
  const auto dir = std::filesystem::temp_directory_path() / "futu_kill_flag_locked";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directory(dir);
  ::chmod(dir.c_str(), 0);  // cannot even stat things inside
  KillSwitch ks;
  EXPECT_TRUE(ks.checkFlagFile((dir / "flag").string()));  // "can't tell" must halt, not pass
  EXPECT_TRUE(ks.tripped());
  ::chmod(dir.c_str(), 0700);
  std::filesystem::remove_all(dir);
}

// --- Promotion log ------------------------------------------------------------------------------

namespace {
std::vector<PromotionEntry> days(int n, bool clean = true) {
  std::vector<PromotionEntry> out;
  for (int i = 1; i <= n; ++i) {
    out.push_back({"2026-09-" + std::string(i < 10 ? "0" : "") + std::to_string(i), clean});
  }
  return out;
}
}  // namespace

TEST(Promotion, NeedsRequiredConsecutiveCleanDays) {
  EXPECT_FALSE(promotionEligible(days(4), 5, "2026-09-05"));
  EXPECT_TRUE(promotionEligible(days(5), 5, "2026-09-05"));
  EXPECT_TRUE(promotionEligible(days(9), 5, "2026-09-09"));
  EXPECT_FALSE(promotionEligible(days(5), 0, "2026-09-05"));
}

TEST(Promotion, DirtyDayInsideWindowBlocks) {
  auto log = days(8);
  log[5].clean = false;  // inside the last 5
  EXPECT_FALSE(promotionEligible(log, 5, "2026-09-08"));
  log[5].clean = true;
  log[2].clean = false;  // older than the window: fine
  EXPECT_TRUE(promotionEligible(log, 5, "2026-09-08"));
}

TEST(Promotion, DuplicateOrOutOfOrderDatesAreNotTrusted) {
  auto log = days(5);
  log[3].date = log[2].date;
  EXPECT_FALSE(promotionEligible(log, 5, "2026-09-05"));
}

TEST(Promotion, InventedAncientDatesCannotPromote) {
  // Five plausible-looking clean lines from years ago must not open the gate.
  std::vector<PromotionEntry> old;
  for (int d = 1; d <= 5; ++d) {
    old.push_back({"2001-01-0" + std::to_string(d), true});
  }
  EXPECT_FALSE(promotionEligible(old, 5, "2026-09-06"));
}

TEST(Promotion, StaleLogIsRejectedEvenIfPerfect) {
  EXPECT_TRUE(promotionEligible(days(5), 5, "2026-09-10"));   // 5 days after the newest entry
  EXPECT_FALSE(promotionEligible(days(5), 5, "2026-09-11"));  // 6 days: too stale
}

TEST(Promotion, FutureDatedEntriesAreRejected) {
  EXPECT_FALSE(promotionEligible(days(5), 5, "2026-09-04"));  // newest entry (09-05) is ahead
}

TEST(Promotion, LargeGapsBetweenEntriesAreNotConsecutiveSessions) {
  auto log = days(5);
  log[2].date = "2026-08-01";  // a month-long hole in the middle
  log[0].date = "2026-07-30";
  log[1].date = "2026-07-31";
  EXPECT_FALSE(promotionEligible(log, 5, "2026-09-05"));
  auto weekend = days(5);  // a normal Fri->Mon gap (3 days) is fine
  weekend[2].date = "2026-09-03";
  weekend[3].date = "2026-09-04";
  weekend[4].date = "2026-09-07";
  EXPECT_TRUE(promotionEligible(weekend, 5, "2026-09-08"));
}

TEST(Promotion, UnparseableTodayFailsClosed) {
  EXPECT_FALSE(promotionEligible(days(5), 5, ""));
  EXPECT_FALSE(promotionEligible(days(5), 5, "not-a-date"));
}

TEST(Promotion, ParseAndFormatRoundTrip) {
  const auto parsed = parsePromotionLog("2026-09-01 clean\n2026-09-02 dirty\n\n");
  ASSERT_TRUE(parsed.ok());
  ASSERT_EQ(parsed.value().size(), 2U);
  EXPECT_TRUE(parsed.value()[0].clean);
  EXPECT_FALSE(parsed.value()[1].clean);
  EXPECT_EQ(formatPromotionEntry(parsed.value()[1]), "2026-09-02 dirty\n");
}

TEST(Promotion, MalformedLogIsRejectedNotPartiallyTrusted) {
  EXPECT_FALSE(parsePromotionLog("2026-09-01 clean\nnonsense\n").ok());
  EXPECT_FALSE(parsePromotionLog("2026-9-1 clean\n").ok());
  EXPECT_FALSE(parsePromotionLog("2026-09-01 maybe\n").ok());
  EXPECT_FALSE(parsePromotionLog("2026-09-01 clean extra\n").ok());
}

TEST(Promotion, ImpossibleCalendarDatesAreRejected) {
  EXPECT_FALSE(parsePromotionLog("2026-13-01 clean\n").ok());
  EXPECT_FALSE(parsePromotionLog("2026-02-30 clean\n").ok());
  EXPECT_FALSE(parsePromotionLog("2026-02-29 clean\n").ok());  // 2026 is not a leap year
  EXPECT_TRUE(parsePromotionLog("2028-02-29 clean\n").ok());   // 2028 is
  EXPECT_FALSE(parsePromotionLog("2026-00-10 clean\n").ok());
  EXPECT_FALSE(parsePromotionLog("1899-01-01 clean\n").ok());
}

TEST(Promotion, OversizedLogIsTreatedAsCorrupt) {
  std::string big;
  while (big.size() <= kMaxPromotionLogBytes) {
    big += "2026-09-01 clean\n";
  }
  EXPECT_FALSE(parsePromotionLog(big).ok());
}

// --- Live gate ----------------------------------------------------------------------------------

namespace {
LiveGateInput validInput() { return testing_support::validLiveInput(123456789); }
}  // namespace

TEST(LiveGate, AllConditionsMetApproves) {
  const auto approval = LiveGate::approveReal(validInput());
  ASSERT_TRUE(approval.ok()) << approval.error().message;
  EXPECT_EQ(approval.value().accId(), 123456789U);
}

TEST(LiveGate, PhraseEmbedsLastFourDigitsOfAccount) {
  EXPECT_EQ(LiveGate::expectedPhrase(123456789), "I-ACCEPT-REAL-MONEY-6789");
  EXPECT_EQ(LiveGate::expectedPhrase(42), "I-ACCEPT-REAL-MONEY-42");
}

TEST(LiveGate, EachMissingConditionAloneBlocksReal) {
  {
    auto in = validInput();
    in.configPhrase = "yes";
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.configPhrase = LiveGate::expectedPhrase(999999);  // phrase for another account
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.envVar = "true";
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.envVar.clear();
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.cliLiveFlag = false;
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.promotion.pop_back();
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.today = "2027-01-01";  // the log is stale relative to today
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.tradeUnlocked = false;
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.startupReconcileClean = false;
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.promotion[4].clean = false;
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.brokerAccounts.clear();  // broker does not know the account
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.brokerAccounts[0].env = TrdEnv::kSimulate;  // same id but it is a paper account
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
  {
    auto in = validInput();
    in.configuredAccId = 0;
    EXPECT_FALSE(LiveGate::approveReal(in).ok());
  }
}

TEST(TradeTarget, SimulateNeedsNoApprovalAndStampsSimulate) {
  const auto target = TradeTarget::simulate(111, opend::TrdMarket::kHK);
  EXPECT_EQ(target.env(), TrdEnv::kSimulate);
  EXPECT_EQ(target.header().env(), TrdEnv::kSimulate);
  EXPECT_EQ(target.header().accId(), 111U);
}

TEST(TradeTarget, RealRequiresApprovalForThatAccount) {
  const auto approval = LiveGate::approveReal(validInput());
  ASSERT_TRUE(approval.ok());
  const auto ok = TradeTarget::real(approval.value(), 123456789, opend::TrdMarket::kHK);
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(ok.value().env(), TrdEnv::kReal);
  // An approval for account A must not authorise trading account B.
  EXPECT_FALSE(TradeTarget::real(approval.value(), 555, opend::TrdMarket::kHK).ok());
}

// Compile-time guarantee: a LiveApproval cannot be created outside LiveGate.
static_assert(!std::is_default_constructible_v<LiveApproval>);
static_assert(!std::is_constructible_v<LiveApproval, std::uint64_t>);

// Compile-time guarantee: nobody can hand-build a header that addresses REAL money.
static_assert(!std::is_default_constructible_v<opend::AccountHeader>);
static_assert(
    !std::is_constructible_v<opend::AccountHeader, TrdEnv, std::uint64_t, opend::TrdMarket>);
