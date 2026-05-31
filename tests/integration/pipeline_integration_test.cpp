#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

#include "futu_trader/api/futu_client.hpp"
#include "futu_trader/execution/order_manager.hpp"
#include "futu_trader/model/mean_reversion_model.hpp"
#include "futu_trader/pipeline/trading_pipeline.hpp"

int main() {
  futu_trader::FutuClient client({});
  client.connect();
  futu_trader::OrderManager om(client);
  futu_trader::RiskEngine risk({1000000, 1000000, 100000, 100, 1.0});
  auto model = std::make_shared<futu_trader::MeanReversionModel>(-0.5, 0.5);

  futu_trader::TradingPipeline pipeline(model, risk, om);
  pipeline.start();
  pipeline.pushTick({"AAPL.US", 12000, 100, std::chrono::system_clock::now()});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pipeline.stop();

  assert(pipeline.metrics().processedTicks >= 1);
  return 0;
}
