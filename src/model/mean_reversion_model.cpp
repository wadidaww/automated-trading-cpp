#include "futu_trader/model/mean_reversion_model.hpp"

namespace futu_trader {

MeanReversionModel::MeanReversionModel(double buyThreshold, double sellThreshold)
    : buy_threshold_(buyThreshold), sell_threshold_(sellThreshold) {}

Signal MeanReversionModel::predict(const FeatureVector& features) const {
  if (features.empty()) {
    return {SignalAction::kHold, 0.0};
  }
  const double zScore = features.front();
  if (zScore <= buy_threshold_) {
    return {SignalAction::kBuy, 0.8};
  }
  if (zScore >= sell_threshold_) {
    return {SignalAction::kSell, 0.8};
  }
  return {SignalAction::kHold, 0.5};
}

}  // namespace futu_trader
