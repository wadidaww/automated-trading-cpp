#pragma once

#include <string>
#include <vector>

#include "futu_trader/model/signal_model.hpp"

namespace futu_trader {

class GradientBoostingModel : public ISignalModel {
 public:
  explicit GradientBoostingModel(std::vector<double> weights);
  Signal Predict(const FeatureVector& features) const override;

  bool Save(const std::string& path) const;
  static GradientBoostingModel Load(const std::string& path);

 private:
  std::vector<double> weights_;
};

}  // namespace futu_trader
