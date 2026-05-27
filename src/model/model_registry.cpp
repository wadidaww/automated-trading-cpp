#include "futu_trader/model/model_registry.hpp"

namespace futu_trader {

void ModelRegistry::SetModel(std::shared_ptr<ISignalModel> model) {
  std::scoped_lock lock(mu_);
  model_ = std::move(model);
}

std::shared_ptr<ISignalModel> ModelRegistry::GetModel() const {
  std::scoped_lock lock(mu_);
  return model_;
}

}  // namespace futu_trader
