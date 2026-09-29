#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <set>
#include <string>

#include "futu_trader/api/futu_client.hpp"
#include "futu_trader/execution/order_manager.hpp"
#include "futu_trader/model/signal_model.hpp"
#include "futu_trader/pipeline/trading_pipeline.hpp"

using namespace futu_trader;

namespace {

class FixedModel : public ISignalModel {
 public:
  explicit FixedModel(SignalAction action) : action_(action) {}
  Signal predict(const FeatureVector&) const override { return {action_, 1.0}; }

 private:
  SignalAction action_;
};

Tick tick(const std::string& sym, Money price) {
  return Tick{sym, price, 100, std::chrono::system_clock::now()};
}

struct Fixture {
  explicit Fixture(RiskConfig risk, SignalAction action, PipelineConfig cfg = {})
      : client({}),
        om(client),
        engine(risk),
        model(std::make_shared<FixedModel>(action)),
        pipeline(model, engine, om, cfg) {
    client.connect();
  }
  FutuClient client;
  OrderManager om;
  RiskEngine engine;
  std::shared_ptr<FixedModel> model;
  TradingPipeline pipeline;
};

RiskConfig roomy() { return {1'000'000, 10'000'000, 100'000, 100, 1.0}; }

}  // namespace

TEST(TradingPipeline, NeedsTwoTicksBeforeSignalling) {
  Fixture f(roomy(), SignalAction::kBuy);
  f.pipeline.start();
  f.pipeline.pushTick(tick("700.HK", 10000));
  f.pipeline.drain();
  EXPECT_EQ(f.pipeline.metrics().generatedSignals, 0U);
  f.pipeline.pushTick(tick("700.HK", 10010));
  f.pipeline.drain();
  const auto m = f.pipeline.metrics();
  EXPECT_EQ(m.processedTicks, 2U);
  EXPECT_EQ(m.generatedSignals, 1U);
  EXPECT_EQ(m.submittedOrders, 1U);
}

TEST(TradingPipeline, HoldSignalPlacesNoOrders) {
  Fixture f(roomy(), SignalAction::kHold);
  f.pipeline.start();
  for (int i = 0; i < 5; ++i) {
    f.pipeline.pushTick(tick("700.HK", 10000 + i));
  }
  f.pipeline.drain();
  EXPECT_EQ(f.pipeline.metrics().submittedOrders, 0U);
  EXPECT_EQ(f.om.openOrderCount(), 0U);
}

TEST(TradingPipeline, RiskUsesRealExposureAndBlocksOnceLimitReached) {
  // Regression: risk used to be called with empty state, so position limits never bound.
  // Cap of 25000 per symbol at price 10000 allows 2 one-lot buys, the 3rd must be rejected.
  RiskConfig risk{25000, 1'000'000, 100'000, 100, 1.0};
  Fixture f(risk, SignalAction::kBuy);
  f.pipeline.start();
  for (int i = 0; i < 6; ++i) {
    f.pipeline.pushTick(tick("700.HK", 10000));
  }
  f.pipeline.drain();
  const auto m = f.pipeline.metrics();
  EXPECT_EQ(m.submittedOrders, 2U);
  EXPECT_EQ(m.riskRejects, 3U);  // 5 signals (ticks 2..6), 2 accepted
}

TEST(TradingPipeline, OpenOrderLimitIsEnforced) {
  RiskConfig risk{1'000'000, 10'000'000, 100'000, 3, 1.0};
  Fixture f(risk, SignalAction::kBuy);
  f.pipeline.start();
  for (int i = 0; i < 10; ++i) {
    f.pipeline.pushTick(tick("700.HK", 10000));
  }
  f.pipeline.drain();
  EXPECT_EQ(f.pipeline.metrics().submittedOrders, 3U);
}

TEST(TradingPipeline, VenueRejectsAreCountedNotSubmitted) {
  Fixture f(roomy(), SignalAction::kBuy);
  f.client.disconnect();
  f.pipeline.start();
  f.pipeline.pushTick(tick("700.HK", 10000));
  f.pipeline.pushTick(tick("700.HK", 10000));
  f.pipeline.drain();
  const auto m = f.pipeline.metrics();
  EXPECT_EQ(m.submittedOrders, 0U);
  EXPECT_EQ(m.venueRejects, 1U);
}

TEST(TradingPipeline, BoundedQueueDropsAndCountsWhenStopped) {
  Fixture f(roomy(), SignalAction::kHold);
  EXPECT_FALSE(f.pipeline.pushTick(tick("700.HK", 1)));  // not started
  EXPECT_EQ(f.pipeline.metrics().droppedTicks, 1U);
  f.pipeline.start();
  f.pipeline.stop();
  EXPECT_FALSE(f.pipeline.pushTick(tick("700.HK", 1)));  // after stop
  EXPECT_EQ(f.pipeline.metrics().droppedTicks, 2U);
}

TEST(TradingPipeline, OrderIdsAreUniqueUnderLoad) {
  Fixture f(RiskConfig{1'000'000'000, 10'000'000'000, 100'000, 1000, 1.0}, SignalAction::kBuy);
  f.pipeline.start();
  for (int i = 0; i < 200; ++i) {
    f.pipeline.pushTick(tick("700.HK", 10000));
  }
  f.pipeline.drain();
  const auto m = f.pipeline.metrics();
  // Each submitted order has a unique idempotency key == order id; duplicates would be refused
  // by the order manager and show up as venue rejects.
  EXPECT_EQ(m.venueRejects, 0U);
  EXPECT_EQ(m.submittedOrders, 199U);
}
