#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

#include "futu_trader/core/types.hpp"
#include "futu_trader/execution/order_manager.hpp"
#include "futu_trader/model/signal_model.hpp"
#include "futu_trader/risk/risk_engine.hpp"

namespace futu_trader {

struct PipelineConfig {
  std::int64_t orderQuantity{1};
  std::size_t queueCapacity{65536};  // ticks beyond this are dropped and counted
  std::size_t featureWindow{20};     // rolling window for the z-score feature
};

struct PipelineMetrics {
  std::size_t processedTicks{0};
  std::size_t generatedSignals{0};  // non-hold signals
  std::size_t submittedOrders{0};
  std::size_t riskRejects{0};
  std::size_t venueRejects{0};
  std::size_t droppedTicks{0};
};

class TradingPipeline {
 public:
  TradingPipeline(std::shared_ptr<ISignalModel> model, RiskEngine& risk, OrderManager& orderManager,
                  PipelineConfig config = {});
  ~TradingPipeline();

  void start();
  void stop();

  /** Returns false if the pipeline is stopped or the queue is full (tick dropped). */
  bool pushTick(const Tick& tick);
  PipelineMetrics metrics() const;

  /** Blocks until every tick pushed so far has been processed. For tests and shutdown. */
  void drain();

 private:
  void worker();
  void handleTick(const Tick& tick);

  std::shared_ptr<ISignalModel> model_;
  RiskEngine& risk_;
  OrderManager& order_manager_;
  PipelineConfig config_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable idleCv_;
  std::queue<Tick> queue_;
  bool busy_{false};
  std::thread worker_;
  std::atomic<bool> running_{false};
  PipelineMetrics metrics_{};

  // Worker-thread-only state (no locking needed).
  std::uint64_t orderSeq_{0};
  std::unordered_map<std::string, std::deque<double>> windows_;
  // Signed notional per symbol from accepted orders. Until broker fills are wired in, accepted
  // orders count as exposure, which is the conservative choice for risk.
  std::unordered_map<std::string, Money> exposure_;
  Money grossExposure_{0};
};

}  // namespace futu_trader
