#include <cassert>
#include <filesystem>
#include <vector>

#include "futu_trader/model/gradient_boosting_model.hpp"
#include "futu_trader/model/mean_reversion_model.hpp"

int main() {
  futu_trader::MeanReversionModel mr(-1.0, 1.0);
  assert(mr.Predict({-2.0}).action == futu_trader::SignalAction::kBuy);
  assert(mr.Predict({2.0}).action == futu_trader::SignalAction::kSell);

  futu_trader::GradientBoostingModel gb({1.0, -0.5});
  const auto sig = gb.Predict({1.0, 0.0});
  assert(sig.action == futu_trader::SignalAction::kBuy || sig.action == futu_trader::SignalAction::kHold);

  const std::string path = (std::filesystem::temp_directory_path() / "gb_model_test.bin").string();
  assert(gb.Save(path));
  auto gb2 = futu_trader::GradientBoostingModel::Load(path);
  assert(gb2.Predict({1.0, 0.0}).action == gb.Predict({1.0, 0.0}).action);
  std::filesystem::remove(path);
  return 0;
}
