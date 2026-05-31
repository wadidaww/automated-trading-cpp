#pragma once

#include <string>
#include <vector>

#include "futu_trader/model/signal_model.hpp"

namespace futu_trader {

class GradientBoostingModel : public ISignalModel {
 public:
  explicit GradientBoostingModel(std::vector<double> weights);
  Signal predict(const FeatureVector& features) const override;

  bool save(const std::string& path) const;
  static GradientBoostingModel load(const std::string& path);

 private:
  std::vector<double> weights_;
};

}  // namespace futu_trader
