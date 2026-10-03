// futu_bench: micro-benchmarks and latency distributions for the hot paths.
//
//   futu_bench [--filter SUBSTR] [--json FILE] [--quick]
//
// Each benchmark times individual operations (or small batches for sub-50ns work) and reports a
// distribution, not just a mean: p50/p99/p99.9/max in nanoseconds, with the clock's own overhead
// subtracted. Build with `cmake --preset bench` (Release). Numbers from WSL2, shared CI runners or
// a laptop on battery are indicative only; gate on dedicated bare metal.

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Qot_UpdateBasicQot.pb.h"
#include "futu_trader/backtest/runner.hpp"
#include "futu_trader/backtest/synthetic.hpp"
#include "futu_trader/core/spsc_ring.hpp"
#include "futu_trader/engine/engine.hpp"
#include "futu_trader/infra/histogram.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/opend/framing.hpp"
#include "futu_trader/opend/proto_ids.hpp"
#include "futu_trader/strategy/strategies.hpp"

using namespace futu_trader;
using SteadyT = std::chrono::steady_clock;

namespace {

struct Row {
  std::string name;
  std::uint64_t samples{0};
  double p50{0};
  double p99{0};
  double p999{0};
  double max{0};
  double mean{0};
  std::string note;
};

std::vector<Row> g_rows;
std::uint64_t g_clockOverheadNs = 0;
bool g_quick = false;

std::uint64_t nowNs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyT::now().time_since_epoch())
          .count());
}

template <typename T>
void doNotOptimize(const T& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

std::size_t scaled(std::size_t n) { return g_quick ? std::max<std::size_t>(n / 20, 200) : n; }

void report(const std::string& name, const infra::LatencyHistogram& h, std::uint64_t subtract,
            const std::string& note = "") {
  const auto sub = [&](std::uint64_t v) {
    return static_cast<double>(v > subtract ? v - subtract : 0);
  };
  Row row;
  row.name = name;
  row.samples = h.count();
  row.p50 = sub(h.percentile(0.50));
  row.p99 = sub(h.percentile(0.99));
  row.p999 = sub(h.percentile(0.999));
  row.max = sub(h.max());
  row.mean = std::max(0.0, h.mean() - static_cast<double>(subtract));
  row.note = note;
  g_rows.push_back(row);
}

// Times every call individually (clock overhead subtracted). For work above ~100 ns.
template <typename F>
void benchEach(const std::string& name, std::size_t iters, F&& op, const std::string& note = "") {
  iters = scaled(iters);
  for (std::size_t i = 0; i < iters / 10; ++i) {
    op(i);  // warm-up: caches, branch predictors, lazy allocations
  }
  infra::LatencyHistogram h;
  for (std::size_t i = 0; i < iters; ++i) {
    const std::uint64_t t0 = nowNs();
    op(i);
    h.record(nowNs() - t0);
  }
  report(name, h, g_clockOverheadNs, note);
}

// Times batches and reports per-operation cost. For work below ~100 ns where the clock would
// dominate.
template <typename F>
void benchBatched(const std::string& name, std::size_t batches, std::size_t batchSize, F&& op,
                  const std::string& note = "") {
  batches = scaled(batches);
  for (std::size_t i = 0; i < batchSize * 20; ++i) {
    op(i);
  }
  infra::LatencyHistogram h;
  for (std::size_t b = 0; b < batches; ++b) {
    const std::uint64_t t0 = nowNs();
    for (std::size_t i = 0; i < batchSize; ++i) {
      op(i);
    }
    h.record((nowNs() - t0 - g_clockOverheadNs) / batchSize);
  }
  report(name, h, 0,
         note + (note.empty() ? "" : "; ") +
             "p50/p99 are percentiles OF BATCH MEANS (per-op over batches of " +
             std::to_string(batchSize) + "): tails are hidden");
}

// Two CPUs on DIFFERENT physical cores. Hyperthread siblings share an L1 cache and execution
// units, so a "cross-thread" measurement between siblings is optimistic (an earlier version of
// this benchmark made exactly that mistake and published a flattering number).
struct CpuPair {
  int producer{2};
  int consumer{4};
  bool distinctCores{false};
  std::string description;
};

CpuPair pickCpuPair() {
  std::vector<std::vector<int>> cores;  // groups of sibling CPUs
  const int total = static_cast<int>(std::thread::hardware_concurrency());
  std::vector<bool> seen(static_cast<std::size_t>(std::max(total, 1)), false);
  for (int cpu = 0; cpu < total; ++cpu) {
    if (seen[static_cast<std::size_t>(cpu)]) {
      continue;
    }
    std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                     "/topology/thread_siblings_list");
    std::string list;
    std::getline(in, list);
    std::vector<int> group;
    std::stringstream parts(list);
    std::string token;
    while (std::getline(parts, token, ',')) {  // forms: "2-3" or "2,3"
      const auto dash = token.find('-');
      const int lo = std::atoi(token.substr(0, dash).c_str());
      const int hi = dash == std::string::npos ? lo : std::atoi(token.substr(dash + 1).c_str());
      for (int c = lo; c <= hi && c < total; ++c) {
        group.push_back(c);
        seen[static_cast<std::size_t>(c)] = true;
      }
    }
    if (group.empty()) {
      group.push_back(cpu);
      seen[static_cast<std::size_t>(cpu)] = true;
    }
    cores.push_back(group);
  }
  CpuPair pair;
  if (cores.size() >= 3) {
    // Skip physical core 0: interrupts and housekeeping tend to land there.
    pair.producer = cores[1].front();
    pair.consumer = cores[2].front();
    pair.distinctCores = true;
  }
  pair.description =
      "cpu" + std::to_string(pair.producer) + " <-> cpu" + std::to_string(pair.consumer) +
      (pair.distinctCores ? " (different physical cores)" : " (could not prove distinct cores!)");
  return pair;
}

const CpuPair& cpus() {
  static const CpuPair pair = pickCpuPair();
  return pair;
}

void pinToCpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);  // best effort
}

// ------------------------------------------------------------------------------------------------

void benchClock() {
  infra::LatencyHistogram h;
  for (int i = 0; i < 200'000; ++i) {
    const std::uint64_t t0 = nowNs();
    h.record(nowNs() - t0);
  }
  g_clockOverheadNs = h.percentile(0.5);
  report("clock_overhead (steady_clock back-to-back)", h, 0, "subtracted from the rows below");
}

void benchFraming() {
  std::vector<std::uint8_t> body(160, 0x5A);
  const auto wire =
      opend::encodeFrame(opend::protoId::kQotUpdateBasicQot, 0, body.data(), body.size());
  benchEach("opend_frame_decode (44B hdr + 160B body, incl. SHA-1)", 200'000, [&](std::size_t) {
    opend::FrameDecoder decoder;
    decoder.feed(wire.data(), wire.size());
    auto frame = decoder.next();
    doNotOptimize(frame);
  });
  benchEach("sha1_160B", 200'000, [&](std::size_t) {
    auto digest = opend::sha1(body.data(), body.size());
    doNotOptimize(digest);
  });
  benchEach("opend_frame_encode (160B body)", 200'000, [&](std::size_t) {
    auto out = opend::encodeFrame(opend::protoId::kQotUpdateBasicQot, 1, body.data(), body.size());
    doNotOptimize(out);
  });

  Qot_UpdateBasicQot::Response push;
  push.set_rettype(0);
  auto* q = push.mutable_s2c()->add_basicqotlist();
  q->mutable_security()->set_market(1);
  q->mutable_security()->set_code("00700");
  q->set_issuspended(false);
  q->set_listtime("2004-06-16");
  q->set_pricespread(0.2);
  q->set_updatetime("2026-09-29 10:00:00");
  q->set_highprice(351);
  q->set_openprice(350);
  q->set_lowprice(349);
  q->set_curprice(350.2);
  q->set_lastcloseprice(350);
  q->set_volume(1000);
  q->set_turnover(1);
  q->set_turnoverrate(0.1);
  q->set_amplitude(0.1);
  const std::string bytes = push.SerializeAsString();
  benchEach(
      "protobuf_parse BasicQot push (" + std::to_string(bytes.size()) + "B)", 200'000,
      [&](std::size_t) {
        Qot_UpdateBasicQot::Response parsed;
        parsed.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()));
        doNotOptimize(parsed);
      },
      "default allocator; an arena would cut this");
}

using Tick = engine::QuoteTick;  // the real payload: one 64-byte cache line

void benchRing() {
  {
    SpscRing<Tick> ring(1024);
    Tick tick;
    benchBatched("spsc_push+pop (same thread, 64B payload)", 20'000, 128, [&](std::size_t i) {
      tick.recvNs = static_cast<std::int64_t>(i);
      ring.tryPush(tick);
      Tick out;
      ring.tryPop(out);
      doNotOptimize(out);
    });
  }
  {
    // Ping-pong: A sends a tick, B echoes it. One-way hop = round trip / 2. The payload is the
    // real 64-byte QuoteTick, and the two threads sit on different physical cores.
    SpscRing<Tick> ab(64);
    SpscRing<Tick> ba(64);
    std::atomic<bool> stop{false};
    std::thread echo([&] {
      pinToCpu(cpus().consumer);
      Tick v;
      while (!stop.load(std::memory_order_relaxed)) {
        if (ab.tryPop(v)) {
          while (!ba.tryPush(v)) {
          }
        }
      }
    });
    pinToCpu(cpus().producer);
    infra::LatencyHistogram h;
    const std::size_t iters = scaled(200'000);
    Tick out;
    for (std::size_t i = 0; i < iters + iters / 10; ++i) {
      Tick payload;
      payload.recvNs = static_cast<std::int64_t>(i);
      const std::uint64_t t0 = nowNs();
      while (!ab.tryPush(payload)) {
      }
      while (!ba.tryPop(out)) {
      }
      if (i >= iters / 10) {
        h.record((nowNs() - t0 - g_clockOverheadNs) / 2);
      }
    }
    stop = true;
    echo.join();
    report("spsc_hop (cross-thread, one way = RTT/2, 64B tick)", h, 0,
           cpus().description + "; busy-polling; tail = host scheduling noise");
  }
}

// Instant in-memory broker for measuring OUR decision path, not a network.
class InstantVenue final : public oms::IVenue {
 public:
  // Called right after an order is accepted: lets a benchmark play the broker's fill push.
  using PlacedHook = std::function<void(const opend::PlaceOrderRequest&, std::uint64_t)>;
  void setOnPlaced(PlacedHook hook) { onPlaced_ = std::move(hook); }

  Result<opend::PlacedOrder> place(const opend::PlaceOrderRequest& request) override {
    const std::uint64_t id = nextId_++;
    if (onPlaced_) {
      onPlaced_(request, id);
    }
    return opend::PlacedOrder{id, "x"};
  }
  Result<bool> cancel(std::uint64_t) override { return true; }
  Result<std::vector<opend::BrokerOrder>> listOrders() override {
    return std::vector<opend::BrokerOrder>{};
  }
  Result<std::vector<opend::BrokerFill>> listFills() override {
    return std::vector<opend::BrokerFill>{};
  }
  Result<std::vector<opend::PositionInfo>> listPositions() override {
    return std::vector<opend::PositionInfo>{};
  }
  Result<opend::FundsInfo> funds() override { return opend::FundsInfo{}; }

 private:
  std::uint64_t nextId_{1};
  PlacedHook onPlaced_;
};

struct OmsRig {
  OmsRig()
      : rate({.maxPerWindow = 100'000'000, .windowMs = 30'000, .reservedForCancels = 0}, clock),
        risk({.maxPositionNotionalMinor = INT64_MAX / 4,
              .maxPortfolioNotionalMinor = INT64_MAX / 4,
              .maxDailyLossMinor = INT64_MAX / 4,
              .maxOpenOrders = 100'000'000,
              .concentrationLimit = 1.0},
             {.priceBandBps = 500,
              .maxQuoteAgeMs = 60'000,
              .maxOrderNotionalMills = INT64_MAX / 4,
              .allowShort = false},
             kill, instruments),
        oms(venue, risk, rate, kill, book, clock, config()) {
    instruments.add({"00700", 100});
    (void)oms.bootstrap();
  }
  static oms::OmsConfig config() {
    oms::OmsConfig c;
    c.sessionEpoch = "BN";
    return c;
  }
  // Submits and immediately completes one order, so the live set stays small while the history
  // of finished orders grows (the realistic long-running situation).
  void submitAndFinish(std::size_t i) {
    const oms::QuoteContext quote{350'000, clock.nowNs(), clock.nowNs()};
    const oms::OrderIntent intent{"k" + std::to_string(i), "00700", Side::kBuy, 100, 350'000};
    const auto result = oms.submit(intent, quote);
    opend::BrokerOrder done;
    done.orderId = 1;
    done.remark = result.clOrdId;
    done.code = "00700";
    done.qty = 100;
    done.fillQty = 100;
    done.status = 11;
    oms.onOrderUpdate(done);
  }
  ManualClock clock;
  execution::KillSwitch kill;
  instrument::InstrumentTable instruments;
  portfolio::PositionBook book;
  InstantVenue venue;
  execution::RateLimiter rate;
  oms::PreTradeRisk risk;
  oms::Oms oms;
};

void benchOms() {
  for (const std::size_t history : {0U, 2000U, 20000U}) {
    OmsRig rig;
    for (std::size_t i = 0; i < history; ++i) {
      rig.submitAndFinish(1'000'000 + i);
    }
    std::size_t counter = 0;
    benchEach(
        "oms_submit (instant venue; history starts at " + std::to_string(history) +
            " orders and grows by 1 per op)",
        3000, [&](std::size_t) { rig.submitAndFinish(counter++); },
        "includes risk chain, record creation, journal, one broker update");
  }
  {
    OmsRig rig;
    const oms::QuoteContext quote{350'000, rig.clock.nowNs(), rig.clock.nowNs()};
    Order order;
    order.symbol = "00700";
    order.side = Side::kBuy;
    order.quantity = 100;
    order.limitPriceMinor = 350'000;
    benchBatched("pretrade_risk_check (accept path)", 20'000, 64, [&](std::size_t) {
      auto verdict = rig.risk.check(order, quote, oms::AccountRiskState{});
      doNotOptimize(verdict);
    });
  }
}

class NullContext final : public strategy::StrategyContext {
 public:
  std::int64_t nowNs() const override { return 0; }
  std::int64_t position(const std::string&) const override { return 0; }
  bool hasLiveOrder(const std::string&) const override { return false; }
  std::vector<std::string> liveOrderIds(const std::string&) const override { return {}; }
  oms::SubmitResult submit(const std::string&, Side, std::int64_t, Money) override {
    ++submits;
    return {};
  }
  Result<bool> cancel(const std::string&) override { return true; }
  std::uint64_t random() override { return 4; }
  std::size_t submits{0};
};

void benchStrategy() {
  backtest::SyntheticConfig cfg;
  cfg.count = 50'000;
  const auto quotes = backtest::generateSyntheticQuotes(cfg);
  strategy::MeanReversion::Params params;
  params.window = 60;
  params.entryZx10 = 1000;  // never enters: measures the pure signal path
  strategy::MeanReversion strat(params);
  NullContext ctx;
  benchEach("strategy_meanreversion_onQuote (window 60, no orders)", 50'000,
            [&](std::size_t i) { strat.onQuote(quotes[i % quotes.size()], ctx); });
}

// A real two-thread run: a producer feeds quotes at a fixed pace into the engine, whose
// busy-polling thread runs the real strategy and OMS. Reports how long a quote waited in the ring
// and how long the engine took to handle it, measured by the engine itself.
void benchEngine() {
  struct Rig {
    Rig()
        : rate({.maxPerWindow = 100'000'000, .windowMs = 30'000, .reservedForCancels = 0}, clock),
          risk({.maxPositionNotionalMinor = INT64_MAX / 4,
                .maxPortfolioNotionalMinor = INT64_MAX / 4,
                .maxDailyLossMinor = INT64_MAX / 4,
                .maxOpenOrders = 100'000'000,
                .concentrationLimit = 1.0},
               {.priceBandBps = 5000,
                .maxQuoteAgeMs = 3'600'000,
                .maxOrderNotionalMills = INT64_MAX / 4,
                .allowShort = false},
               kill, instruments),
          oms(venue, risk, rate, kill, book, clock, config()) {
      instruments.add({"00700", 100});
      (void)oms.bootstrap();
      // Play the broker: every accepted order is filled at once, as the push stream would report
      // it, so the live set stays realistically small (real systems cap open orders at tens).
      venue.setOnPlaced([this](const opend::PlaceOrderRequest& req, std::uint64_t id) {
        opend::BrokerFill fill;
        fill.fillId = "F" + std::to_string(id);
        fill.orderId = id;
        fill.code = req.code;
        fill.side = req.side;
        fill.qty = req.qty;
        fill.priceMills = req.priceMills;
        opend::BrokerOrder done;
        done.orderId = id;
        done.remark = req.remark;
        done.code = req.code;
        done.qty = req.qty;
        done.fillQty = req.qty;
        done.status = 11;
        oms.onFill(fill);
        oms.onOrderUpdate(done);
      });
    }
    static oms::OmsConfig config() {
      oms::OmsConfig c;
      c.sessionEpoch = "EB";
      return c;
    }
    SteadyClock clock;
    execution::KillSwitch kill;
    instrument::InstrumentTable instruments;
    portfolio::PositionBook book;
    InstantVenue venue;
    execution::RateLimiter rate;
    oms::PreTradeRisk risk;
    oms::Oms oms;
  };

  const auto run = [&](const std::string& label, strategy::IStrategy& strat, std::int64_t paceNs,
                       std::size_t count) {
    Rig rig;
    engine::EngineConfig cfg;
    cfg.busyPoll = true;
    cfg.ringCapacity = 8192;
    cfg.engineCpu = cpus().consumer;
    engine::Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, cfg);
    engine.start();
    pinToCpu(cpus().producer);
    backtest::SyntheticConfig sc;
    sc.count = 4096;
    const auto quotes = backtest::generateSyntheticQuotes(sc);
    const auto settled = [&] {
      const auto& st = engine.stats();
      return st.processed.load() + st.staleSkipped.load() + st.dropped.load() +
                 st.badSymbol.load() + st.rejectedAfterStop.load() >=
             st.received.load();
    };
    // Warm-up: first-use paths, cold caches and lazily built maps must not pollute the tails.
    {
      std::uint64_t warm = nowNs();
      for (std::size_t i = 0; i < 4000; ++i) {
        while (nowNs() < warm) {
        }
        warm += static_cast<std::uint64_t>(paceNs);
        engine.onQuote(quotes[i % quotes.size()]);
      }
      while (!settled()) {
        std::this_thread::yield();
      }
      engine.resetLatencyStats();
    }
    // Measured phase. Each quote is stamped with its SCHEDULED arrival time, so if this sender
    // is delayed (common on a shared host) the delay is counted as latency instead of vanishing:
    // otherwise a stall would silently remove the very samples that show it (coordinated omission).
    std::uint64_t next = nowNs();
    for (std::size_t i = 0; i < count; ++i) {
      while (nowNs() < next) {
      }
      engine.onQuoteAt(quotes[i % quotes.size()], static_cast<std::int64_t>(next));
      next += static_cast<std::uint64_t>(paceNs);
    }
    while (!settled()) {
      std::this_thread::yield();
    }
    engine.stop();
    const auto& st = engine.stats();
    const std::string note = std::to_string(st.processed.load()) + " handled, " +
                             std::to_string(st.dropped.load()) + " dropped, ring high-water " +
                             std::to_string(st.ringHighWater.load());
    report("engine_queue_wait (" + label + ")", st.queueNs, g_clockOverheadNs, note);
    report("engine_handle (" + label + ")", st.handleNs, g_clockOverheadNs, note);
  };

  const std::size_t quotesToSend = scaled(100'000);
  {
    strategy::MeanReversion::Params p;
    p.window = 60;
    p.entryZx10 = 100'000;  // never trades: the pure quote -> decision path
    strategy::MeanReversion strat(p);
    run("quote->decision, 200k quotes/s", strat, 5000, quotesToSend);
  }
  {
    struct AlwaysBuy final : strategy::IStrategy {
      void onQuote(const QuoteEvent& q, strategy::StrategyContext& ctx) override {
        ctx.submit("00700", Side::kBuy, 100, q.ask);
      }
    } strat;
    run("quote->ORDER->fill through risk+OMS, 20k quotes/s", strat, 50'000, scaled(20'000));
  }
}

void benchHistogram() {
  infra::LatencyHistogram h;
  benchBatched(
      "histogram_record", 20'000, 256, [&](std::size_t i) { h.record(100 + (i % 5000)); },
      "relaxed atomics, 4 per call");
}

void benchBacktestThroughput() {
  backtest::SyntheticConfig cfg;
  cfg.count = g_quick ? 5000 : 40'000;
  const auto quotes = backtest::generateSyntheticQuotes(cfg);
  strategy::MeanReversion::Params params;
  params.window = 60;
  params.entryZx10 = 15;
  strategy::MeanReversion strat(params);
  const std::uint64_t t0 = nowNs();
  const auto result = backtest::runBacktest(backtest::defaultBacktestConfig(), quotes, strat);
  const double seconds = static_cast<double>(nowNs() - t0) / 1e9;
  Row row;
  row.name = "backtest_throughput (full OMS + simulated broker + reconciles)";
  row.samples = quotes.size();
  row.mean = seconds * 1e9 / static_cast<double>(quotes.size());
  row.p50 = row.mean;
  row.note = std::to_string(static_cast<long>(static_cast<double>(quotes.size()) / seconds)) +
             " events/s, " + std::to_string(result.fills.size()) + " fills";
  g_rows.push_back(row);
}

void print() {
  std::printf("\n%-78s %9s %9s %9s %9s %9s\n", "benchmark (ns)", "p50", "p99", "p99.9", "max",
              "mean");
  for (const auto& r : g_rows) {
    std::printf("%-78s %9.0f %9.0f %9.0f %9.0f %9.0f", r.name.c_str(), r.p50, r.p99, r.p999, r.max,
                r.mean);
    if (!r.note.empty()) {
      std::printf("   (%s)", r.note.c_str());
    }
    std::printf("\n");
  }
}

std::string json() {
  std::ostringstream out;
  out << "[\n";
  for (std::size_t i = 0; i < g_rows.size(); ++i) {
    const auto& r = g_rows[i];
    out << "  {\"name\": \"" << r.name << "\", \"samples\": " << r.samples
        << ", \"p50_ns\": " << r.p50 << ", \"p99_ns\": " << r.p99 << ", \"p999_ns\": " << r.p999
        << ", \"max_ns\": " << r.max << ", \"mean_ns\": " << r.mean << "}"
        << (i + 1 < g_rows.size() ? "," : "") << "\n";
  }
  out << "]\n";
  return out.str();
}

}  // namespace

int main(int argc, char** argv) {
  std::string filter;
  std::string jsonPath;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--quick") {
      g_quick = true;
    } else if (arg == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (arg == "--json" && i + 1 < argc) {
      jsonPath = argv[++i];
    } else {
      std::cerr << "usage: futu_bench [--filter SUBSTR] [--json FILE] [--quick]\n";
      return 2;
    }
  }
  const auto wanted = [&](const char* group) {
    return filter.empty() || std::string(group).find(filter) != std::string::npos;
  };
  benchClock();
  if (wanted("framing")) {
    benchFraming();
  }
  if (wanted("ring")) {
    benchRing();
  }
  if (wanted("oms")) {
    benchOms();
  }
  if (wanted("strategy")) {
    benchStrategy();
  }
  if (wanted("engine")) {
    benchEngine();
  }
  if (wanted("histogram")) {
    benchHistogram();
  }
  if (wanted("backtest")) {
    benchBacktestThroughput();
  }
  print();
  if (!jsonPath.empty()) {
    std::ofstream(jsonPath) << json();
  }
  return 0;
}
