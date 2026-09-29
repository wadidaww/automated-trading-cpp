#include "futu_trader/model/model_trainer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace futu_trader {

GradientBoostingModel ModelTrainer::train(const std::vector<FeatureVector>& features,
                                          const std::vector<int>& labels) const {
  if (features.empty() || labels.empty()) {
    return GradientBoostingModel({});
  }
  const std::size_t dims = features.front().size();
  std::vector<double> weights(dims, 0.0);
  for (std::size_t i = 0; i < features.size(); ++i) {
    const double y = labels[i] > 0 ? 1.0 : -1.0;
    for (std::size_t d = 0; d < dims; ++d) {
      weights[d] += y * features[i][d];
    }
  }
  const double scale = 1.0 / static_cast<double>(features.size());
  for (double& w : weights) {
    w *= scale;
  }
  return GradientBoostingModel(std::move(weights));
}

TrainingMetrics ModelTrainer::evaluate(const std::vector<double>& pnl, int truePositive,
                                       int falsePositive, int falseNegative) const {
  TrainingMetrics m;
  if (!pnl.empty()) {
    const double mean =
        std::accumulate(pnl.begin(), pnl.end(), 0.0) / static_cast<double>(pnl.size());
    double var = 0.0;
    for (double r : pnl) {
      const double d = r - mean;
      var += d * d;
    }
    const double stddev = std::sqrt(var / static_cast<double>(pnl.size()));
    m.sharpe = stddev == 0.0 ? 0.0 : mean / stddev;
    double peak = 0.0;
    double equity = 0.0;
    for (double r : pnl) {
      equity += r;
      peak = std::max(peak, equity);
      if (peak > 0.0) {
        m.maxDrawdown = std::max(m.maxDrawdown, (peak - equity) / peak);
      }
    }
  }
  m.precision = (truePositive + falsePositive) == 0
                    ? 0.0
                    : static_cast<double>(truePositive) / (truePositive + falsePositive);
  m.recall = (truePositive + falseNegative) == 0
                 ? 0.0
                 : static_cast<double>(truePositive) / (truePositive + falseNegative);
  return m;
}

}  // namespace futu_trader
