#include "futu_trader/model/mean_reversion_model.hpp"

namespace futu_trader {

MeanReversionModel::MeanReversionModel(double buy_threshold, double sell_threshold)
    : buy_threshold_(buy_threshold), sell_threshold_(sell_threshold) {}

Signal MeanReversionModel::Predict(const FeatureVector& features) const {
  if (features.empty()) {
    return {SignalAction::kHold, 0.0};
  }
  const double z_score = features.front();
  if (z_score <= buy_threshold_) {
    return {SignalAction::kBuy, 0.8};
  }
  if (z_score >= sell_threshold_) {
    return {SignalAction::kSell, 0.8};
  }
  return {SignalAction::kHold, 0.5};
}

}  // namespace futu_trader
