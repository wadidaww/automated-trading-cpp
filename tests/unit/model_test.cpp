#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "futu_trader/model/gradient_boosting_model.hpp"
#include "futu_trader/model/mean_reversion_model.hpp"
#include "futu_trader/model/model_trainer.hpp"

using namespace futu_trader;

TEST(MeanReversionModel, BuysLowSellsHigh) {
  MeanReversionModel mr(-1.0, 1.0);
  EXPECT_EQ(mr.predict({-2.0}).action, SignalAction::kBuy);
  EXPECT_EQ(mr.predict({2.0}).action, SignalAction::kSell);
  EXPECT_EQ(mr.predict({0.0}).action, SignalAction::kHold);
}

TEST(GradientBoostingModel, ScoreAboveThresholdBuys) {
  GradientBoostingModel gb({1.0, -0.5});
  EXPECT_EQ(gb.predict({1.0, 0.0}).action, SignalAction::kBuy);
  EXPECT_EQ(gb.predict({-1.0, 0.0}).action, SignalAction::kSell);
  EXPECT_EQ(gb.predict({0.1, 0.0}).action, SignalAction::kHold);
}

class ModelFile : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "futu_gb_model_test.bin").string();
  }
  void TearDown() override { std::filesystem::remove(path_); }
  std::string path_;
};

TEST_F(ModelFile, RoundTrips) {
  GradientBoostingModel gb({1.0, -0.5});
  ASSERT_TRUE(gb.save(path_));
  const auto loaded = GradientBoostingModel::load(path_);
  EXPECT_EQ(loaded.predict({1.0, 0.0}).action, SignalAction::kBuy);
}

TEST_F(ModelFile, CorruptHeaderDoesNotAllocateHugeBuffer) {
  // Regression: the header of the old placeholder file ("trained-model") decoded to a huge size.
  {
    std::ofstream out(path_, std::ios::binary);
    out << "trained-model";
  }
  const auto loaded = GradientBoostingModel::load(path_);
  EXPECT_EQ(loaded.predict({10.0}).action, SignalAction::kHold);
}

TEST_F(ModelFile, TruncatedPayloadIsRejected) {
  {
    std::ofstream out(path_, std::ios::binary);
    const std::uint64_t size = 4;
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    const double one = 1.0;
    out.write(reinterpret_cast<const char*>(&one), sizeof(one));
  }
  EXPECT_EQ(GradientBoostingModel::load(path_).predict({10.0}).action, SignalAction::kHold);
}

TEST(ModelTrainer, RejectsMismatchedInputs) {
  ModelTrainer trainer;
  // labels shorter than features
  auto m1 = trainer.train({{1.0}, {2.0}}, {1});
  EXPECT_EQ(m1.predict({10.0}).action, SignalAction::kHold);
  // ragged rows
  auto m2 = trainer.train({{1.0, 2.0}, {3.0}}, {1, 1});
  EXPECT_EQ(m2.predict({10.0, 10.0}).action, SignalAction::kHold);
}

TEST(ModelTrainer, LearnsDirection) {
  ModelTrainer trainer;
  auto model = trainer.train({{2.0}, {3.0}, {-2.0}, {-3.0}}, {1, 1, -1, -1});
  EXPECT_EQ(model.predict({2.0}).action, SignalAction::kBuy);
  EXPECT_EQ(model.predict({-2.0}).action, SignalAction::kSell);
}
