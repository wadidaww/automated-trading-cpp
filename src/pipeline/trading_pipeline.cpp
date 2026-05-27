#include "futu_trader/pipeline/trading_pipeline.hpp"

namespace futu_trader {

TradingPipeline::TradingPipeline(std::shared_ptr<ISignalModel> model, RiskEngine& risk,
                                 OrderManager& order_manager)
    : model_(std::move(model)), risk_(risk), order_manager_(order_manager) {}

TradingPipeline::~TradingPipeline() { Stop(); }

void TradingPipeline::Start() {
  if (running_.exchange(true)) {
    return;
  }
  worker_ = std::thread(&TradingPipeline::Worker, this);
}

void TradingPipeline::Stop() {
  if (!running_.exchange(false)) {
    return;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void TradingPipeline::PushTick(const Tick& tick) {
  {
    std::scoped_lock lock(mu_);
    queue_.push(tick);
  }
  cv_.notify_one();
}

PipelineMetrics TradingPipeline::Metrics() const {
  std::scoped_lock lock(mu_);
  return metrics_;
}

void TradingPipeline::Worker() {
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
    ++metrics_.processed_ticks;
    lock.unlock();

    const FeatureVector features = {static_cast<double>(tick.price_minor) / 10000.0};
    const Signal signal = model_->Predict(features);
    if (signal.action == SignalAction::kHold) {
      continue;
    }
    Order order;
    order.order_id = tick.symbol + "-" + std::to_string(metrics_.processed_ticks);
    order.symbol = tick.symbol;
    order.quantity = 1;
    order.limit_price_minor = tick.price_minor;
    order.idempotency_key = order.order_id;

    const bool allowed = risk_.CanPlace(order, {}, 0, 0, 0);
    if (allowed) {
      order_manager_.Submit(order);
      std::scoped_lock guard(mu_);
      ++metrics_.generated_signals;
    }
  }
}

}  // namespace futu_trader
