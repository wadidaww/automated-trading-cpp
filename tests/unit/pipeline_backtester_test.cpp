#include <cassert>
#include <chrono>
#include <memory>
#include <thread>

#include "futu_trader/api/futu_client.hpp"
#include "futu_trader/evaluation/backtester.hpp"
#include "futu_trader/execution/order_manager.hpp"
#include "futu_trader/model/mean_reversion_model.hpp"
#include "futu_trader/pipeline/trading_pipeline.hpp"

int main() {
  futu_trader::FutuClient client({});
  client.Connect();
  futu_trader::OrderManager om(client);
  futu_trader::RiskEngine risk({1000000, 1000000, 100000, 100, 1.0});
  auto model = std::make_shared<futu_trader::MeanReversionModel>(-0.5, 0.5);

  futu_trader::TradingPipeline pipeline(model, risk, om);
  pipeline.Start();
  pipeline.PushTick({"700.HK", 10000, 100, std::chrono::system_clock::now()});
  pipeline.PushTick({"700.HK", 5000, 100, std::chrono::system_clock::now()});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pipeline.Stop();
  assert(pipeline.Metrics().processed_ticks >= 2);

  futu_trader::Backtester backtester;
  const auto metrics = backtester.Run({{"700.HK", 10000, 1, std::chrono::system_clock::now()},
                                       {"700.HK", 10200, 1, std::chrono::system_clock::now()}},
                                      *model);
  assert(metrics.total_return != 0.0 || metrics.max_drawdown >= 0.0);
  return 0;
}
