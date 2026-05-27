#pragma once

#include <vector>

#include "futu_trader/model/gradient_boosting_model.hpp"

namespace futu_trader {

struct TrainingMetrics {
  double sharpe{0.0};
  double maxDrawdown{0.0};
  double precision{0.0};
  double recall{0.0};
};

class ModelTrainer {
 public:
  GradientBoostingModel train(const std::vector<FeatureVector>& features,
                              const std::vector<int>& labels) const;
  TrainingMetrics evaluate(const std::vector<double>& pnl, int truePositive, int falsePositive,
                           int falseNegative) const;
};

}  // namespace futu_trader
