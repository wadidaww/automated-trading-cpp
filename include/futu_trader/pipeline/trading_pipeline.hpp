#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "futu_trader/core/types.hpp"
#include "futu_trader/execution/order_manager.hpp"
#include "futu_trader/model/signal_model.hpp"
#include "futu_trader/risk/risk_engine.hpp"

namespace futu_trader {

struct PipelineMetrics {
  std::size_t processedTicks{0};
  std::size_t generatedSignals{0};
};

class TradingPipeline {
 public:
  TradingPipeline(std::shared_ptr<ISignalModel> model, RiskEngine& risk,
                  OrderManager& orderManager);
  ~TradingPipeline();

  void start();
  void stop();
  void pushTick(const Tick& tick);
  PipelineMetrics metrics() const;

 private:
  void worker();

  std::shared_ptr<ISignalModel> model_;
  RiskEngine& risk_;
  OrderManager& order_manager_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::queue<Tick> queue_;
  std::thread worker_;
  std::atomic<bool> running_{false};
  PipelineMetrics metrics_{};
};

}  // namespace futu_trader
