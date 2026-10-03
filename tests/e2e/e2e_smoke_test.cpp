#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

TEST(E2eSmoke, HealthCheckExitsZero) {
  const std::string cmd = std::string(FUTU_TRADER_BIN) + " --health-check >/dev/null 2>&1";
  EXPECT_EQ(std::system(cmd.c_str()), 0);
}
