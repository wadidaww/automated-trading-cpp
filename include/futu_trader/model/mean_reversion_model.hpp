#pragma once

#include "futu_trader/model/signal_model.hpp"

namespace futu_trader {

class MeanReversionModel : public ISignalModel {
 public:
  MeanReversionModel(double buyThreshold, double sellThreshold);
  Signal predict(const FeatureVector& features) const override;

 private:
  double buy_threshold_;
  double sell_threshold_;
};

}  // namespace futu_trader
