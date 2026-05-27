#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "futu_trader/model/signal_model.hpp"

namespace futu_trader {

class ModelRegistry {
 public:
  void setModel(std::shared_ptr<ISignalModel> model);
  std::shared_ptr<ISignalModel> getModel() const;

 private:
  mutable std::mutex mu_;
  std::shared_ptr<ISignalModel> model_;
};

}  // namespace futu_trader
