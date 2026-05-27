#pragma once

#include "futu_trader/core/types.hpp"

namespace futu_trader {

class ISignalModel {
 public:
  virtual ~ISignalModel() = default;
  virtual Signal Predict(const FeatureVector& features) const = 0;
};

}  // namespace futu_trader
