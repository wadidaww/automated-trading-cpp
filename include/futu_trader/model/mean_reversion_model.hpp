#pragma once

#include "futu_trader/model/signal_model.hpp"

namespace futu_trader {

class MeanReversionModel : public ISignalModel {
 public:
  MeanReversionModel(double buy_threshold, double sell_threshold);
  Signal Predict(const FeatureVector& features) const override;

 private:
  double buy_threshold_;
  double sell_threshold_;
};

}  // namespace futu_trader
