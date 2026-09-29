#include "futu_trader/pipeline/trading_pipeline.hpp"

#include <cstdlib>
#include <vector>

#include "futu_trader/data/data_normalizer.hpp"

namespace futu_trader {

TradingPipeline::TradingPipeline(std::shared_ptr<ISignalModel> model, RiskEngine& risk,
                                 OrderManager& orderManager, PipelineConfig config)
    : model_(std::move(model)), risk_(risk), order_manager_(orderManager), config_(config) {}

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
  // Take the lock between flipping the flag and notifying. Without it the worker can evaluate its
  // wait predicate (running_ still true), then miss the notify and sleep forever (lost wakeup).
  { std::scoped_lock lock(mu_); }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

bool TradingPipeline::pushTick(const Tick& tick) {
  {
    std::scoped_lock lock(mu_);
    if (!running_.load() || queue_.size() >= config_.queueCapacity) {
      ++metrics_.droppedTicks;
      return false;
    }
    queue_.push(tick);
  }
  cv_.notify_one();
  return true;
}

PipelineMetrics TradingPipeline::metrics() const {
  std::scoped_lock lock(mu_);
  return metrics_;
}

void TradingPipeline::drain() {
  std::unique_lock lock(mu_);
  idleCv_.wait(lock, [&] { return queue_.empty() && !busy_; });
}

void TradingPipeline::worker() {
  while (true) {
    Tick tick;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [&] { return !running_.load() || !queue_.empty(); });
      if (queue_.empty()) {
        break;  // stopped and fully drained
      }
      tick = std::move(queue_.front());
      queue_.pop();
      busy_ = true;
    }

    handleTick(tick);

    {
      std::scoped_lock lock(mu_);
      ++metrics_.processedTicks;
      busy_ = false;
    }
    idleCv_.notify_all();
  }
}

void TradingPipeline::handleTick(const Tick& tick) {
  auto& window = windows_[tick.symbol];
  window.push_back(static_cast<double>(tick.priceMinor));
  if (window.size() > config_.featureWindow) {
    window.pop_front();
  }
  if (window.size() < 2) {
    return;
  }

  const std::vector<double> prices(window.begin(), window.end());
  const FeatureVector features = {DataNormalizer::rollingZScore(prices)};
  const Signal signal = model_->predict(features);
  if (signal.action == SignalAction::kHold) {
    return;
  }

  Order order;
  order.orderId = tick.symbol + "-" + std::to_string(++orderSeq_);
  order.symbol = tick.symbol;
  order.side = signal.action == SignalAction::kBuy ? Side::kBuy : Side::kSell;
  order.quantity = config_.orderQuantity;
  order.limitPriceMinor = tick.priceMinor;
  order.idempotencyKey = order.orderId;

  {
    std::scoped_lock lock(mu_);
    ++metrics_.generatedSignals;
  }

  const RiskReject verdict =
      risk_.check(order, exposure_, grossExposure_, 0, order_manager_.openOrderCount());
  if (verdict != RiskReject::kOk) {
    std::scoped_lock lock(mu_);
    ++metrics_.riskRejects;
    return;
  }

  if (!order_manager_.submit(order)) {
    std::scoped_lock lock(mu_);
    ++metrics_.venueRejects;
    return;
  }

  Money& symbolExposure = exposure_[tick.symbol];
  const Money before = std::llabs(symbolExposure);
  symbolExposure += sideSign(order.side) * order.limitPriceMinor * order.quantity;
  grossExposure_ += std::llabs(symbolExposure) - before;

  std::scoped_lock lock(mu_);
  ++metrics_.submittedOrders;
}

}  // namespace futu_trader
