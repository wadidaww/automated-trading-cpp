#include "futu_trader/pipeline/trading_pipeline.hpp"

namespace futu_trader {

TradingPipeline::TradingPipeline(std::shared_ptr<ISignalModel> model, RiskEngine& risk,
                                 OrderManager& orderManager)
    : model_(std::move(model)), risk_(risk), order_manager_(orderManager) {}

TradingPipeline::~TradingPipeline() { stop(); }

void TradingPipeline::start() {
  if (running_.exchange(true)) {
    return;
  }
  worker_ = std::thread(&TradingPipeline::worker, this);
}

void TradingPipeline::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void TradingPipeline::pushTick(const Tick& tick) {
  {
    std::scoped_lock lock(mu_);
    queue_.push(tick);
  }
  cv_.notify_one();
}

PipelineMetrics TradingPipeline::metrics() const {
  std::scoped_lock lock(mu_);
  return metrics_;
}

void TradingPipeline::worker() {
  while (true) {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [&] { return !running_.load() || !queue_.empty(); });
    if (!running_.load() && queue_.empty()) {
      break;
    }
    if (queue_.empty()) {
      continue;
    }
    Tick tick = queue_.front();
    queue_.pop();
    ++metrics_.processedTicks;
    lock.unlock();

    const FeatureVector features = {static_cast<double>(tick.priceMinor) / 10000.0};
    const Signal signal = model_->predict(features);
    if (signal.action == SignalAction::kHold) {
      continue;
    }
    Order order;
    order.orderId = tick.symbol + "-" + std::to_string(metrics_.processedTicks);
    order.symbol = tick.symbol;
    order.quantity = 1;
    order.limitPriceMinor = tick.priceMinor;
    order.idempotencyKey = order.orderId;

    const bool allowed = risk_.canPlace(order, {}, 0, 0, 0);
    if (allowed) {
      order_manager_.submit(order);
      std::scoped_lock guard(mu_);
      ++metrics_.generatedSignals;
    }
  }
}

}  // namespace futu_trader
