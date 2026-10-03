#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>

#include "futu_trader/core/clock.hpp"
#include "futu_trader/core/spsc_ring.hpp"
#include "futu_trader/execution/kill_switch.hpp"
#include "futu_trader/infra/histogram.hpp"
#include "futu_trader/market/quote.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/portfolio/position_book.hpp"
#include "futu_trader/strategy/oms_context.hpp"
#include "futu_trader/strategy/strategy.hpp"

namespace futu_trader::engine {

inline constexpr std::size_t kCacheLine = 64;
inline constexpr std::size_t kMaxSymbolLen = 7;  // HK codes are 5 digits; "00700" fits with room

/**
 * A quote as it crosses threads: exactly one cache line, trivially copyable, symbol inline, so
 * passing it through the ring never allocates and a ring slot never shares a line with its
 * neighbour (the producer writing slot i would otherwise contend with the consumer reading slot
 * i-1). Symbols longer than kMaxSymbolLen are refused at the boundary and counted, never truncated.
 */
struct alignas(kCacheLine) QuoteTick {
  std::int64_t recvNs{0};  // our clock when the quote entered the engine
  std::int64_t tsNs{0};    // the quote's own timestamp
  Money bid{0};
  Money ask{0};
  Money last{0};
  std::int64_t bidSize{0};
  std::int64_t askSize{0};
  std::array<char, kMaxSymbolLen + 1> symbol{};
};
static_assert(std::is_trivially_copyable_v<QuoteTick>);
static_assert(sizeof(QuoteTick) == kCacheLine, "one quote per cache line");

bool toTick(const QuoteEvent& quote, std::int64_t recvNs, QuoteTick& out);
QuoteEvent toEvent(const QuoteTick& tick);

struct EngineConfig {
  std::size_t ringCapacity{4096};
  /**
   * A quote that waited longer than this in the queue is discarded unhandled (and counted). After
   * a stall the backlog is stale state: acting on it would trade on prices that are long gone.
   * 0 disables the check.
   */
  std::int64_t maxQuoteAgeNs{1'000'000'000};
  /** How often the engine thread runs housekeeping (halt servicing, kill-flag watchdog). */
  std::int64_t housekeepingEveryNs{500'000'000};
  /** If non-empty, the presence of this file halts trading (an external watchdog's lever). */
  std::string killFlagPath;
  /** If > 0, a separate thread reconciles with the broker this often (never on the hot thread). */
  std::int64_t reconcileEveryNs{0};
  /** Burn a core polling for lowest latency instead of sleeping when idle. */
  bool busyPoll{false};
  std::chrono::microseconds idleSleep{200};
  std::uint64_t seed{1};
  /** Best-effort thread placement (-1 = leave to the scheduler); results are in EngineStats. */
  int engineCpu{-1};
  int reconcilerCpu{-1};
  int engineRealtimePriority{0};  // SCHED_FIFO priority 1..99, 0 = off
};

/**
 * Counters are grouped by WRITER and each group starts on its own cache line, so the market-data
 * thread, the engine thread and the reconciler thread never invalidate each other's lines.
 *
 * Accounting identity, valid once the ring is empty and no thread is mid-call:
 *   received == processed + staleSkipped + dropped + badSymbol + rejectedAfterStop
 */
struct EngineStats {
  // ---- written by the producer (market-data) thread ----
  alignas(kCacheLine) std::atomic<std::uint64_t> received{0};  // every onQuote call
  std::atomic<std::uint64_t> dropped{0};            // ring full: the consumer could not keep up
  std::atomic<std::uint64_t> badSymbol{0};          // empty or over-long symbol: refused
  std::atomic<std::uint64_t> rejectedAfterStop{0};  // offered after stop(): refused
  // ---- written by the engine thread ----
  alignas(kCacheLine) std::atomic<std::uint64_t> processed{0};  // quotes the strategy has seen
  std::atomic<std::uint64_t> staleSkipped{0};  // waited past maxQuoteAgeNs: discarded unhandled
  std::atomic<std::uint64_t> strategyFaults{0};
  std::atomic<std::uint64_t> ringHighWater{0};  // deepest backlog seen at the start of a poll
  std::atomic<bool> engineFailed{false};        // an unexpected exception stopped quote handling
  std::atomic<bool> enginePinned{false};        // thread placement actually took effect
  std::atomic<bool> engineRealtime{false};
  infra::LatencyHistogram queueNs;   // enqueue -> picked up by the engine thread
  infra::LatencyHistogram handleNs;  // time the strategy + OMS took per quote
  // ---- written by the reconciler thread ----
  alignas(kCacheLine) std::atomic<std::uint64_t> reconciles{0};
  std::atomic<std::uint64_t> reconcileProblems{0};  // incomplete, with drift, or threw
  std::atomic<bool> reconcilerPinned{false};
};

/**
 * The real-time core: quotes are pushed (wait-free) from the market-data thread into a ring; ONE
 * engine thread pops them and runs strategy -> OMS; a housekeeping beat on that thread services
 * halts and the kill-flag file; an optional reconciler thread talks to the broker on its own so
 * a slow reconciliation can never stall quote handling.
 *
 * Behaviour worth knowing:
 *  - Backpressure: a full ring drops the NEWEST quote and counts it; quotes that waited longer
 *    than maxQuoteAgeNs are skipped when dequeued. Together these mean the strategy never acts on
 *    a stale backlog. `dropped` and `staleSkipped` should alarm: the strategy was blind.
 *    Order and fill events never go through this ring.
 *  - THE ENGINE THREAD BLOCKS ON THE BROKER. strategy -> OMS -> venue.place() is a synchronous
 *    network call (tens of ms through OpenD), as are cancel-all halts. While it blocks, quotes
 *    queue (and go stale, and are skipped). That is acceptable for the intraday strategies this
 *    system targets, and the price of a simple, deterministic order lifecycle. A dedicated order
 *    sender thread would remove it at the cost of an asynchronous submit API.
 *  - A strategy that throws is disabled for good, the kill switch trips and resting orders are
 *    cancelled. Any other exception on the engine thread does the same and sets `engineFailed`:
 *    the process must never die with orders working unattended.
 *  - An externally tripped kill switch (flag file, operator) cancels resting orders once.
 *  - stop() drains every quote already accepted, then refuses further ones.
 *
 * Threading contract: onQuote() from exactly one producer thread at a time; poll()/housekeeping()
 * only from the engine thread (or from a test that has NOT called start()). start()/stop() may be
 * called from any thread but are serialized. The producer must stop calling onQuote before the
 * Engine is destroyed. Debug builds assert the single-producer/single-consumer rule.
 */
class Engine {
 public:
  Engine(oms::Oms& oms, strategy::IStrategy& strategy, const portfolio::PositionBook& book,
         const Clock& clock, execution::KillSwitch& killSwitch, EngineConfig config);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /** Producer side. Wait-free. Returns false if the quote was dropped or refused. */
  bool onQuote(const QuoteEvent& quote) { return onQuoteAt(quote, clock_.nowNs()); }
  /**
   * Same, but with an explicit receive time. Production code uses onQuote(); a load generator
   * passes the SCHEDULED send time so that its own stalls count as latency instead of hiding
   * (coordinated omission).
   */
  bool onQuoteAt(const QuoteEvent& quote, std::int64_t recvNs);

  /** Consumer side: handles up to `max` queued quotes on the calling thread. */
  std::size_t poll(std::size_t max = 256);
  /** Services halts and the kill-flag watchdog. Cheap when nothing is wrong. */
  void housekeeping();

  void start();
  void stop();

  /** Clears the latency histograms (e.g. after a warm-up phase). */
  void resetLatencyStats();

  const EngineStats& stats() const { return stats_; }
  strategy::OmsContext& context() { return context_; }
  bool strategyDisabled() const { return strategyDisabled_.load(); }

 private:
  void run();
  void reconcileLoop();
  void handle(const QuoteTick& tick);
  void fault(const std::string& why) noexcept;

  oms::Oms& oms_;
  strategy::IStrategy& strategy_;
  const Clock& clock_;
  execution::KillSwitch& kill_;
  EngineConfig config_;
  strategy::OmsContext context_;
  SpscRing<QuoteTick> ring_;
  EngineStats stats_;

  std::mutex controlMu_;  // serialises start()/stop()
  std::atomic<bool> running_{false};
  std::atomic<bool> stopped_{false};  // set by stop(): onQuote refuses until start() again
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> strategyDisabled_{false};
  bool cancelledOnTrip_{false};  // engine-thread only
  std::thread engineThread_;
  std::thread reconcilerThread_;
  std::mutex wakeMu_;
  std::condition_variable wakeCv_;
#ifndef NDEBUG
  std::atomic<bool> producerBusy_{false};  // debug-only misuse detection
  std::atomic<bool> consumerBusy_{false};
#endif
};

}  // namespace futu_trader::engine
