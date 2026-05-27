#pragma once

#include <vector>

#include "futu_trader/model/gradient_boosting_model.hpp"

namespace futu_trader {

struct TrainingMetrics {
  double sharpe{0.0};
  double max_drawdown{0.0};
  double precision{0.0};
  double recall{0.0};
};

class ModelTrainer {
 public:
  GradientBoostingModel Train(const std::vector<FeatureVector>& features,
                              const std::vector<int>& labels) const;
  TrainingMetrics Evaluate(const std::vector<double>& pnl, int true_positive, int false_positive,
                           int false_negative) const;
};

}  // namespace futu_trader
