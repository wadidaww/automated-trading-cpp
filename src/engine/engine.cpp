#include "futu_trader/engine/engine.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <exception>

#include "futu_trader/infra/thread_tuning.hpp"

namespace futu_trader::engine {

namespace {

#ifndef NDEBUG
// Asserts that a section is never entered by two threads at once (the SPSC contract).
class SingleThreadScope {
 public:
  explicit SingleThreadScope(std::atomic<bool>& busy) : busy_(busy) {
    const bool wasBusy = busy_.exchange(true);
    assert(!wasBusy && "SPSC contract violated: two threads inside the same side of the engine");
    static_cast<void>(wasBusy);
  }
  ~SingleThreadScope() { busy_.store(false); }
  SingleThreadScope(const SingleThreadScope&) = delete;
  SingleThreadScope& operator=(const SingleThreadScope&) = delete;

 private:
  std::atomic<bool>& busy_;
};
#endif

}  // namespace

bool toTick(const QuoteEvent& quote, std::int64_t recvNs, QuoteTick& out) {
  if (quote.symbol.empty() || quote.symbol.size() > kMaxSymbolLen) {
    return false;
  }
  out.recvNs = recvNs;
  out.tsNs = quote.tsNs;
  out.bid = quote.bid;
  out.ask = quote.ask;
  out.last = quote.last;
  out.bidSize = quote.bidSize;
  out.askSize = quote.askSize;
  out.symbol.fill('\0');
  std::memcpy(out.symbol.data(), quote.symbol.data(), quote.symbol.size());
  return true;
}

QuoteEvent toEvent(const QuoteTick& tick) {
  QuoteEvent quote;
  quote.tsNs = tick.tsNs;
  quote.symbol.assign(tick.symbol.data());  // <= 7 chars: stays inside the small-string buffer
  quote.bid = tick.bid;
  quote.ask = tick.ask;
  quote.last = tick.last;
  quote.bidSize = tick.bidSize;
  quote.askSize = tick.askSize;
  return quote;
}

Engine::Engine(oms::Oms& oms, strategy::IStrategy& strategy, const portfolio::PositionBook& book,
               const Clock& clock, execution::KillSwitch& killSwitch, EngineConfig config)
    : oms_(oms),
      strategy_(strategy),
      clock_(clock),
      kill_(killSwitch),
      config_(std::move(config)),
      context_(oms, book, clock, config_.seed),
      ring_(config_.ringCapacity) {
  context_.setKeyPrefix(config_.intentPrefix);
}

Engine::~Engine() { stop(); }

bool Engine::onQuoteAt(const QuoteEvent& quote, std::int64_t recvNs) {
#ifndef NDEBUG
  const SingleThreadScope scope(producerBusy_);
#endif
  stats_.received.fetch_add(1, std::memory_order_relaxed);
  if (stopped_.load(std::memory_order_acquire)) {
    stats_.rejectedAfterStop.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  QuoteTick tick;
  if (!toTick(quote, recvNs, tick)) {
    stats_.badSymbol.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!ring_.tryPush(tick)) {
    stats_.dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;  // nothing here touches the consumer's cache lines
}

void Engine::fault(const std::string& why) noexcept {
  // Last line of defence: try the full halt, fall back to just tripping the switch, never throw.
  try {
    oms_.haltAndCancelAll(why);
  } catch (...) {
    try {
      kill_.trip(why);
    } catch (...) {
    }
  }
}

void Engine::handle(const QuoteTick& tick) {
  const std::int64_t start = clock_.nowNs();
  const std::int64_t waited = std::max<std::int64_t>(0, start - tick.recvNs);
  stats_.queueNs.record(static_cast<std::uint64_t>(waited));
  if (config_.maxQuoteAgeNs > 0 && waited > config_.maxQuoteAgeNs) {
    stats_.staleSkipped.fetch_add(1, std::memory_order_relaxed);
    return;  // stale state is worse than none: never let the strategy act on it
  }
  const QuoteEvent quote = toEvent(tick);
  context_.observe(quote);
  oms_.onMark(quote.symbol, quote.mid());
  if (!strategyDisabled_.load(std::memory_order_relaxed)) {
    try {
      strategy_.onQuote(quote, context_);
    } catch (const std::exception& error) {
      strategyDisabled_.store(true);
      stats_.strategyFaults.fetch_add(1, std::memory_order_relaxed);
      fault(std::string("strategy threw: ") + error.what());
    } catch (...) {
      strategyDisabled_.store(true);
      stats_.strategyFaults.fetch_add(1, std::memory_order_relaxed);
      fault("strategy threw an unknown exception");
    }
  }
  stats_.processed.fetch_add(1, std::memory_order_relaxed);
  stats_.handleNs.record(
      static_cast<std::uint64_t>(std::max<std::int64_t>(0, clock_.nowNs() - start)));
}

std::size_t Engine::poll(std::size_t max) {
#ifndef NDEBUG
  const SingleThreadScope scope(consumerBusy_);
#endif
  // One read of the producer's line per poll (not per quote): the backlog depth we found.
  const std::uint64_t depth = ring_.sizeApprox();
  if (depth > stats_.ringHighWater.load(std::memory_order_relaxed)) {
    stats_.ringHighWater.store(depth, std::memory_order_relaxed);
  }
  std::size_t handled = 0;
  QuoteTick tick;
  while (handled < max && ring_.tryPop(tick)) {
    try {
      handle(tick);
    } catch (...) {
      // Something other than the strategy threw (e.g. allocation failure in bookkeeping).
      stats_.engineFailed.store(true);
      strategyDisabled_.store(true);
      fault("engine thread caught an unexpected exception");
    }
    ++handled;
  }
  return handled;
}

void Engine::housekeeping() {
  if (!config_.killFlagPath.empty()) {
    kill_.checkFlagFile(config_.killFlagPath);
  }
  if (kill_.tripped()) {
    if (!cancelledOnTrip_) {
      // Tripped from outside (flag file, operator): pull resting orders, once. requestHalt() only
      // schedules it; serviceHalt() below does the work without re-sending cancels that an
      // automatic halt (possibly on the reconciler thread) has just sent.
      oms_.requestHalt("kill switch: " + kill_.reason());
      cancelledOnTrip_ = true;
    }
    oms_.serviceHalt();  // no-op unless a halt is pending
  } else {
    cancelledOnTrip_ = false;  // reset by a human: arm the next trip
  }
}

void Engine::start() {
  std::scoped_lock control(controlMu_);
  if (running_.load()) {
    return;
  }
  stopRequested_.store(false);
  stopped_.store(false);
  running_.store(true);
  engineThread_ = std::thread(&Engine::run, this);
  if (config_.reconcileEveryNs > 0) {
    reconcilerThread_ = std::thread(&Engine::reconcileLoop, this);
  }
}

void Engine::stop() {
  std::scoped_lock control(controlMu_);
  if (!running_.load()) {
    return;
  }
  stopped_.store(true, std::memory_order_release);  // refuse new quotes; drain what we accepted
  {
    std::scoped_lock lock(wakeMu_);
    stopRequested_.store(true);
  }
  wakeCv_.notify_all();
  if (engineThread_.joinable()) {
    engineThread_.join();
  }
  if (reconcilerThread_.joinable()) {
    reconcilerThread_.join();
  }
  running_.store(false);
}

void Engine::resetLatencyStats() {
  stats_.queueNs.reset();
  stats_.handleNs.reset();
}

void Engine::run() {
  try {
    if (config_.engineCpu >= 0) {
      stats_.enginePinned.store(infra::pinCurrentThreadToCpu(config_.engineCpu));
    }
    if (config_.engineRealtimePriority > 0) {
      stats_.engineRealtime.store(infra::setCurrentThreadRealtime(config_.engineRealtimePriority));
    }
    std::int64_t lastHousekeeping = clock_.nowNs();
    while (!stopRequested_.load(std::memory_order_acquire)) {
      const std::size_t handled = poll(256);
      const std::int64_t now = clock_.nowNs();
      if (now - lastHousekeeping >= config_.housekeepingEveryNs) {
        housekeeping();
        lastHousekeeping = now;
      }
      if (handled == 0 && !config_.busyPoll) {
        std::this_thread::sleep_for(config_.idleSleep);
      }
    }
    while (poll(256) > 0) {
      // Drain: every quote we accepted is handled before the thread exits.
    }
    housekeeping();
  } catch (...) {
    stats_.engineFailed.store(true);
    strategyDisabled_.store(true);
    fault("engine thread terminated by an unexpected exception");
  }
}

void Engine::reconcileLoop() {
  if (config_.reconcilerCpu >= 0) {
    stats_.reconcilerPinned.store(infra::pinCurrentThreadToCpu(config_.reconcilerCpu));
  }
  const auto period = std::chrono::nanoseconds(config_.reconcileEveryNs);
  std::unique_lock lock(wakeMu_);
  while (!stopRequested_.load()) {
    if (wakeCv_.wait_for(lock, period, [&] { return stopRequested_.load(); })) {
      return;
    }
    lock.unlock();
    try {
      const auto report = oms_.reconcile();  // talks to the broker: never on the engine thread
      stats_.reconciles.fetch_add(1, std::memory_order_relaxed);
      if (!report.clean()) {
        stats_.reconcileProblems.fetch_add(1, std::memory_order_relaxed);
      }
    } catch (...) {
      stats_.reconciles.fetch_add(1, std::memory_order_relaxed);
      stats_.reconcileProblems.fetch_add(1, std::memory_order_relaxed);
      fault("reconciliation threw an unexpected exception");
    }
    lock.lock();
  }
}

}  // namespace futu_trader::engine
