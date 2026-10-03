// Allocation-counting tests. These replace global operator new/delete, so they live in their own
// executable and are not built under sanitizers (which hook the allocator themselves).
//
// The design claim being enforced: the QUOTE path (market data in -> strategy decision, no order)
// performs zero heap allocations in steady state. The ORDER path allocates (strings, journal
// entries), but is bounded by the exchange's own rate limit (~15 orders / 30 s), so it is measured
// and capped here rather than eliminated.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "futu_trader/core/spsc_ring.hpp"
#include "futu_trader/engine/engine.hpp"
#include "futu_trader/infra/histogram.hpp"
#include "futu_trader/strategy/strategies.hpp"

namespace {
thread_local bool g_counting = false;
thread_local std::size_t g_allocations = 0;

struct AllocScope {
  AllocScope() {
    g_allocations = 0;
    g_counting = true;
  }
  ~AllocScope() { g_counting = false; }
  std::size_t count() const { return g_allocations; }
};
}  // namespace

void* operator new(std::size_t size) {
  if (g_counting) {
    ++g_allocations;
  }
  if (void* p = std::malloc(size == 0 ? 1 : size)) {
    return p;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, std::align_val_t align) {
  if (g_counting) {
    ++g_allocations;
  }
  void* p = nullptr;
  if (posix_memalign(&p, static_cast<std::size_t>(align), size == 0 ? 1 : size) != 0) {
    throw std::bad_alloc();
  }
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }

using namespace futu_trader;

namespace {

class StubVenue final : public oms::IVenue {
 public:
  Result<opend::PlacedOrder> place(const opend::PlaceOrderRequest&) override {
    return opend::PlacedOrder{nextId++, "x"};
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
  std::uint64_t nextId{1};
};

struct Rig {
  Rig()
      : rate({.maxPerWindow = 100000, .windowMs = 30'000, .reservedForCancels = 1000}, clock),
        risk({.maxPositionNotionalMinor = INT64_MAX / 4,
              .maxPortfolioNotionalMinor = INT64_MAX / 4,
              .maxDailyLossMinor = INT64_MAX / 4,
              .maxOpenOrders = 100000,
              .concentrationLimit = 1.0},
             {.priceBandBps = 5000,
              .maxQuoteAgeMs = 3'600'000,
              .maxOrderNotionalMills = INT64_MAX / 4,
              .allowShort = false},
             kill, instruments),
        oms(venue, risk, rate, kill, book, clock, config()) {
    instruments.add({"00700", 100});
    (void)oms.bootstrap();
  }
  static oms::OmsConfig config() {
    oms::OmsConfig c;
    c.sessionEpoch = "AL";
    return c;
  }
  ManualClock clock;
  execution::KillSwitch kill;
  instrument::InstrumentTable instruments;
  portfolio::PositionBook book;
  StubVenue venue;
  execution::RateLimiter rate;
  oms::PreTradeRisk risk;
  oms::Oms oms;
};

QuoteEvent quoteAt(std::int64_t i) {
  const Money bid = 350'000 + ((i * 7) % 20) * 200;
  return {i, "00700", bid, bid + 200, bid, 1000, 1000};
}

}  // namespace

TEST(Allocations, NegativeControlTheCounterReallyCountsAllocations) {
  // A "0" result is only evidence if the instrument can see allocations at all.
  AllocScope scope;
  auto boxed = std::make_unique<int>(1);
  std::vector<int> grown;
  grown.reserve(100);
  std::string longText(200, 'x');
  EXPECT_GE(scope.count(), 3U);
  EXPECT_EQ(*boxed, 1);
  EXPECT_EQ(grown.capacity(), 100U);
  EXPECT_EQ(longText.size(), 200U);
}

TEST(Allocations, TheQuotePathAllocatesNothingInSteadyState) {
  Rig rig;
  strategy::MeanReversion::Params params;
  params.window = 60;
  params.entryZx10 = 100'000;  // never enters: a pure market-data -> decision path
  strategy::MeanReversion strat(params);
  engine::Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, engine::EngineConfig{});

  for (std::int64_t i = 1; i <= 600; ++i) {  // warm-up: maps, window buffer, first-use paths
    ASSERT_TRUE(engine.onQuote(quoteAt(i)));
    engine.poll();
  }
  std::vector<QuoteEvent> quotes;
  for (std::int64_t i = 601; i <= 5600; ++i) {
    quotes.push_back(quoteAt(i));
  }

  std::size_t allocations = 0;
  for (const auto& q : quotes) {
    AllocScope scope;
    engine.onQuote(q);
    engine.poll();
    allocations += scope.count();
  }
  EXPECT_EQ(allocations, 0U) << "the quote path must not touch the heap";
  EXPECT_EQ(engine.stats().processed.load(), 5600U);
}

TEST(Allocations, SeveralSymbolsInterleavedStayAllocationFreeOnceEachHasBeenSeen) {
  Rig rig;
  strategy::MeanReversion::Params params;
  params.window = 60;
  params.entryZx10 = 100'000;
  strategy::MeanReversion strat(params);
  engine::Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, engine::EngineConfig{});
  const std::vector<std::string> symbols{"00700", "09988", "00005", "AAPL"};
  const auto quoteFor = [&](std::int64_t i) {
    QuoteEvent q = quoteAt(i);
    q.symbol = symbols[static_cast<std::size_t>(i) % symbols.size()];
    return q;
  };
  for (std::int64_t i = 1; i <= 800; ++i) {  // warm-up covers every symbol
    ASSERT_TRUE(engine.onQuote(quoteFor(i)));
    engine.poll();
  }
  std::vector<QuoteEvent> quotes;
  for (std::int64_t i = 801; i <= 4800; ++i) {
    quotes.push_back(quoteFor(i));
  }
  std::size_t allocations = 0;
  for (const auto& q : quotes) {
    AllocScope scope;
    engine.onQuote(q);
    engine.poll();
    allocations += scope.count();
  }
  EXPECT_EQ(allocations, 0U);
}

TEST(Allocations, TheBoundaryOfTheClaimAFirstSeenSymbolDoesAllocate) {
  // The zero-allocation guarantee is for symbols that have been seen before. Documenting where it
  // stops keeps the claim honest: the first quote of a new symbol grows several maps.
  Rig rig;
  strategy::MeanReversion::Params params;
  params.entryZx10 = 100'000;
  strategy::MeanReversion strat(params);
  engine::Engine engine(rig.oms, strat, rig.book, rig.clock, rig.kill, engine::EngineConfig{});
  for (std::int64_t i = 1; i <= 100; ++i) {
    engine.onQuote(quoteAt(i));
    engine.poll();
  }
  QuoteEvent fresh = quoteAt(101);
  fresh.symbol = "NEWSYM";
  AllocScope scope;
  engine.onQuote(fresh);
  engine.poll();
  EXPECT_GT(scope.count(), 0U);
}

TEST(Allocations, RingHistogramAndRiskCheckAreAllocationFree) {
  SpscRing<std::int64_t> ring(64);
  infra::LatencyHistogram histogram;
  Rig rig;
  const oms::QuoteContext quote{350'000, 0, 0};
  Order order;
  order.symbol = "00700";
  order.side = Side::kBuy;
  order.quantity = 100;
  order.limitPriceMinor = 350'000;
  const oms::AccountRiskState state;

  AllocScope scope;
  for (int i = 0; i < 1000; ++i) {
    std::int64_t out = 0;
    ring.tryPush(i);
    ring.tryPop(out);
    histogram.record(static_cast<std::uint64_t>(100 + i));
    (void)rig.risk.check(order, quote, state);
  }
  EXPECT_EQ(scope.count(), 0U);
}

TEST(Allocations, TheOrderPathAllocatesButIsBoundedAndWeKnowTheNumber) {
  Rig rig;
  const oms::QuoteContext quote{350'000, 0, 0};
  for (int i = 0; i < 50; ++i) {  // warm-up
    rig.oms.submit({"w" + std::to_string(i), "00700", Side::kBuy, 100, 350'000}, quote);
  }
  std::size_t total = 0;
  constexpr int kOrders = 200;
  for (int i = 0; i < kOrders; ++i) {
    const oms::OrderIntent intent{"k" + std::to_string(i), "00700", Side::kBuy, 100, 350'000};
    AllocScope scope;
    const auto result = rig.oms.submit(intent, quote);
    total += scope.count();
    ASSERT_EQ(result.status, oms::SubmitStatus::kAccepted);
  }
  const double perOrder = static_cast<double>(total) / kOrders;
  std::printf("[ alloc    ] order path: %.1f heap allocations per accepted order\n", perOrder);
  // A regression guard, not a target: orders are rate-limited by the exchange to a handful per
  // second at most, so this cost is irrelevant to throughput. If it balloons, look at why.
  EXPECT_LT(perOrder, 40.0);  // measured ~14: leaves headroom but catches a real regression
}
