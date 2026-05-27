#include "futu_trader/model/gradient_boosting_model.hpp"

#include <cmath>
#include <fstream>

namespace futu_trader {

GradientBoostingModel::GradientBoostingModel(std::vector<double> weights)
    : weights_(std::move(weights)) {}

Signal GradientBoostingModel::Predict(const FeatureVector& features) const {
  if (weights_.empty() || features.empty()) {
    return {SignalAction::kHold, 0.0};
  }
  const std::size_t n = std::min(weights_.size(), features.size());
  double score = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    score += weights_[i] * features[i];
  }
  const double confidence = 1.0 / (1.0 + std::exp(-std::fabs(score)));
  if (score > 0.5) {
    return {SignalAction::kBuy, confidence};
  }
  if (score < -0.5) {
    return {SignalAction::kSell, confidence};
  }
  return {SignalAction::kHold, confidence};
}

bool GradientBoostingModel::Save(const std::string& path) const {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  std::uint64_t size = weights_.size();
  out.write(reinterpret_cast<const char*>(&size), sizeof(size));
  out.write(reinterpret_cast<const char*>(weights_.data()),
            static_cast<std::streamsize>(sizeof(double) * size));
  return static_cast<bool>(out);
}

GradientBoostingModel GradientBoostingModel::Load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return GradientBoostingModel({});
  }
  std::uint64_t size = 0;
  in.read(reinterpret_cast<char*>(&size), sizeof(size));
  if (!in) {
    return GradientBoostingModel({});
  }
  std::vector<double> weights(size, 0.0);
  in.read(reinterpret_cast<char*>(weights.data()), static_cast<std::streamsize>(sizeof(double) * size));
  if (!in) {
    return GradientBoostingModel({});
  }
  return GradientBoostingModel(std::move(weights));
}

}  // namespace futu_trader
