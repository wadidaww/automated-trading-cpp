#include "futu_trader/app/promotion.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "futu_trader/oms/live_gate.hpp"
#include "test_support.hpp"

using namespace futu_trader;
using namespace futu_trader::app;

namespace {

struct TempLog : testing_support::ScopedPath {
  TempLog() : ScopedPath("futu_promo"), path(str()) {}
  std::string read() const { return testing_support::slurp(path); }
  void write(const std::string& text) const { testing_support::spit(path, text); }
  std::string path;
};

}  // namespace

TEST(Promotion, CreatesTheLogOwnerOnlyAndAppendsDaysInOrder) {
  TempLog log;
  ASSERT_TRUE(recordPromotionDay(log.path, "2026-10-01", true).ok());
  ASSERT_TRUE(recordPromotionDay(log.path, "2026-10-02", true).ok());
  ASSERT_TRUE(recordPromotionDay(log.path, "2026-10-05", false).ok());
  EXPECT_EQ(log.read(), "2026-10-01 clean\n2026-10-02 clean\n2026-10-05 dirty\n");
  struct stat info {};
  ASSERT_EQ(::stat(log.path.c_str(), &info), 0);
  EXPECT_EQ(info.st_mode & 0777, 0600U);
  EXPECT_FALSE(std::filesystem::exists(log.path + ".tmp"));  // no leftovers
}

TEST(Promotion, ASecondSessionOnTheSameDayMergesAndDirtyIsSticky) {
  TempLog log;
  ASSERT_TRUE(recordPromotionDay(log.path, "2026-10-01", true).ok());
  ASSERT_TRUE(recordPromotionDay(log.path, "2026-10-01", true).ok());
  EXPECT_EQ(log.read(), "2026-10-01 clean\n");
  ASSERT_TRUE(recordPromotionDay(log.path, "2026-10-01", false).ok());
  EXPECT_EQ(log.read(), "2026-10-01 dirty\n");
  ASSERT_TRUE(
      recordPromotionDay(log.path, "2026-10-01", true).ok());  // a clean rerun cannot erase it
  EXPECT_EQ(log.read(), "2026-10-01 dirty\n");
}

TEST(Promotion, TheResultIsAlwaysSomethingTheLiveGateCanParse) {
  TempLog log;
  for (const auto* day : {"2026-09-28", "2026-09-29", "2026-09-30", "2026-10-01", "2026-10-02"}) {
    ASSERT_TRUE(recordPromotionDay(log.path, day, true).ok());
  }
  const auto parsed = oms::parsePromotionLog(log.read());
  ASSERT_TRUE(parsed.ok());
  EXPECT_TRUE(oms::promotionEligible(parsed.value(), 5, "2026-10-03"));
}

TEST(Promotion, RefusesToGoBackwardsInTimeOrWriteABadDate) {
  TempLog log;
  ASSERT_TRUE(recordPromotionDay(log.path, "2026-10-05", true).ok());
  const auto before = log.read();
  EXPECT_FALSE(
      recordPromotionDay(log.path, "2026-10-01", true).ok());  // earlier than the last entry
  EXPECT_FALSE(recordPromotionDay(log.path, "2026-13-45", true).ok());
  EXPECT_FALSE(recordPromotionDay(log.path, "yesterday", true).ok());
  EXPECT_EQ(log.read(), before);
}

TEST(Promotion, AnUnparseableExistingLogIsLeftUntouched) {
  TempLog log;
  log.write("2026-10-01 clean\nthis line is garbage\n");
  const auto before = log.read();
  EXPECT_FALSE(recordPromotionDay(log.path, "2026-10-02", true).ok());
  EXPECT_EQ(log.read(), before);
}

TEST(Promotion, AnUnwritableLocationIsAnErrorNotASilentSuccess) {
  EXPECT_FALSE(recordPromotionDay("/nonexistent-dir/promotion.log", "2026-10-01", true).ok());
}
