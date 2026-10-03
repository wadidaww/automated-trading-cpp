#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "futu_trader/core/clock.hpp"
#include "futu_trader/core/result.hpp"
#include "futu_trader/execution/kill_switch.hpp"
#include "futu_trader/execution/rate_limiter.hpp"
#include "futu_trader/oms/order_state.hpp"
#include "futu_trader/oms/pre_trade.hpp"
#include "futu_trader/oms/venue.hpp"
#include "futu_trader/portfolio/fees.hpp"
#include "futu_trader/portfolio/position_book.hpp"

namespace futu_trader::oms {

struct OmsConfig {
  /** Unique per process start (e.g. a timestamp). Becomes part of every ClOrdId. */
  std::string sessionEpoch;
  portfolio::HkFeeSchedule fees;
  /** How long an ambiguous submit may stay unresolved before "not at broker" is believed. */
  std::int64_t ambiguityGraceMs{3000};
  /** If set, reconciliation also compares broker cash to baseline + traded cash flow. */
  std::optional<Money> cashToleranceMills;
  /** Consecutive failed reconciliations before the kill switch trips. */
  int maxFailedReconciles{3};
  /**
   * An ambiguous submit is only declared "never existed" after the grace period AND this many
   * consecutive complete order listings that all lack it. One listing can simply be behind.
   */
  int absenceListingsRequired{3};
  /** A cancel with no confirmation is resent after this long. Halt always resends. */
  std::int64_t cancelRetryMs{2000};
  /** Held positions whose mark price is older than this block new exposure (fail closed). */
  std::int64_t markMaxAgeMs{60'000};
  /**
   * In-memory journal entries kept (oldest dropped first). Durable history belongs in the WAL;
   * an unbounded in-memory log is a slow memory leak in a process that runs for weeks.
   */
  std::size_t journalRetain{200'000};
  /**
   * Capacity reserved up front for order bookkeeping. Growing a hash map or vector at a power of
   * two rehashes/copies everything in one go, which shows up as a multi-millisecond stall on the
   * engine thread. Sized for a busy day; exceeding it still works, just with a growth hiccup.
   */
  std::size_t reserveOrders{50'000};
  /**
   * If true, submit() refuses (kNotDurable) unless a durable-submit sink is installed. A live
   * process must set it: without the sink orders would go out with no write-ahead log.
   */
  bool requireDurable{false};
};

/** A strategy's request. `intentKey` is its idempotency key: one key never yields two orders. */
struct OrderIntent {
  std::string intentKey;
  std::string symbol;
  Side side{Side::kBuy};
  std::int64_t qty{0};
  Money priceMills{0};
};

struct OrderRecord {
  std::string clOrdId;
  std::string intentKey;  // empty for orders adopted from the broker
  std::string symbol;
  Side side{Side::kBuy};
  std::int64_t qty{0};
  Money priceMills{0};
  OmsState state{OmsState::kNew};
  std::uint64_t venueOrderId{0};
  std::int64_t filledQty{0};
  std::int64_t sentAtNs{0};
  bool external{false};      // not placed by this process (adopted from the broker)
  bool presumedDead{false};  // declared "never existed" by absence; any later sighting halts
  int missedListings{0};     // consecutive complete listings that did not contain it
  std::int64_t cancelSentAtNs{0};
  std::uint64_t seq{0};  // creation order; keys the live-order index
  std::string detail;    // last reject / anomaly explanation
};

enum class SubmitStatus : std::uint8_t {
  kAccepted,           // venue accepted; order is working (or pending its ack)
  kRejectedByRisk,     // never sent; see SubmitResult::risk
  kRejectedByVenue,    // broker definitively refused; key is released for retry
  kAmbiguous,          // outcome unknown; intent stays blocked until reconciled
  kDuplicate,          // this intent key already produced an order
  kBlockedUnresolved,  // this intent key has an unresolved (Unknown) order
  kNotReady,           // bootstrap() has not succeeded yet
  kInvalid,
  kNotDurable,  // the write-ahead log refused the intent: nothing was sent (fail closed)
  kCount_,      // not a status: array sizing
};

struct SubmitResult {
  SubmitStatus status{SubmitStatus::kInvalid};
  std::string clOrdId;
  RiskReject risk{RiskReject::kOk};
  std::string detail;
};

/** One order intent as written ahead of the send; see oms/journal_codec.hpp. */
struct DurableSubmit;  // DurableSubmit::tsNs is whatever clock the SINK stamps; the OMS fills it
                       // with its own (monotonic) clock, so a sink whose records outlive the
                       // process must overwrite it with wall time, and restoreIntents(notBeforeNs)
                       // compares in that same timebase (see src/app/application.cpp).

/** Lock-free counters for metrics. Monotonic; read from any thread. */
struct OmsStats {
  std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(SubmitStatus::kCount_)> submits{};
  // Indexed by RiskReject (kept as raw size so this header need not include the enum's definition).
  std::array<std::atomic<std::uint64_t>, 32> riskRejects{};
  std::atomic<std::uint64_t> durableFailures{0};
  std::atomic<std::uint64_t> restoredIntents{0};
  std::atomic<std::uint64_t> staleUpdates{0};  // late "working" pushes after the order moved on
};

enum class DriftKind : std::uint8_t {
  kOrderMissingAtBroker,
  kOrderStatusUnknown,
  kPositionMismatch,
  kCashMismatch,
};

struct Drift {
  DriftKind kind{DriftKind::kPositionMismatch};
  std::string detail;
};

struct ReconcileReport {
  bool complete{false};  // false if any broker query failed (no conclusions were drawn)
  std::string error;
  std::size_t resolvedUnknown{0};     // ambiguous submits settled either way
  std::size_t statusCatchUps{0};      // order states advanced from the broker's list
  std::size_t missedFillsApplied{0};  // fills found in the fill list that pushes never delivered
  std::size_t adoptedExternal{0};     // live broker orders we did not place
  std::vector<Drift> drifts;          // unexplained differences: these halt trading
  bool halted{false};
  bool clean() const { return complete && drifts.empty(); }
};

struct HaltReport {
  std::size_t cancelRequested{0};
  std::size_t cancelFailed{0};
  std::size_t unresolvedWithoutVenueId{0};  // could not be cancelled: needs a human
};

enum class JournalKind : std::uint8_t {
  kSubmitRequested,
  kRiskReject,
  kPlaced,
  kVenueReject,
  kAmbiguous,
  kStateChange,
  kCancelRequested,
  kFill,
  kAnomaly,
  kReconcile,
  kHalt,
  kBootstrap,
};

struct JournalEntry {
  std::int64_t tsNs{0};
  JournalKind kind{JournalKind::kAnomaly};
  std::string clOrdId;
  std::string detail;
};

/**
 * Order management: risk -> rate limit -> venue, with the order state machine, idempotency,
 * ambiguity handling and broker reconciliation.
 *
 * Thread model: all methods are thread-safe. The venue is only ever called with no lock held, so
 * a slow gateway cannot block push handlers. onOrderUpdate/onFill are meant to be called from
 * the OpenD push thread and never call the venue.
 */
class Oms {
 public:
  Oms(IVenue& venue, PreTradeRisk& risk, execution::RateLimiter& rate,
      execution::KillSwitch& killSwitch, portfolio::PositionBook& book, const Clock& clock,
      OmsConfig config);
  ~Oms();
  Oms(const Oms&) = delete;
  Oms& operator=(const Oms&) = delete;

  /**
   * Loads broker positions and live orders. Trading is refused until this succeeds, because an
   * empty book against an account that already holds positions makes every limit meaningless.
   */
  Result<bool> bootstrap();

  SubmitResult submit(const OrderIntent& intent, const QuoteContext& quote);
  Result<bool> cancel(const std::string& clOrdId);
  /**
   * Trips the kill switch and cancels every live order, resending cancels that were never
   * confirmed. Anything the cancel budget cannot cover stays pending: serviceHalt() finishes it.
   */
  HaltReport haltAndCancelAll(const std::string& reason);
  /**
   * Graceful shutdown: cancels every live order WITHOUT tripping the kill switch (a planned stop is
   * not an incident, and a tripped switch would need a human reset on the next start). Orders
   * without a venue id (unknown outcome) cannot be cancelled and are counted for the caller.
   */
  HaltReport cancelAllLive();
  /**
   * Trips the kill switch and schedules a cancel-all WITHOUT talking to the venue; serviceHalt()
   * carries it out. For callers that want a halt but must not block (or must not double-send
   * cancels that another thread already sent).
   */
  void requestHalt(const std::string& reason);
  /**
   * Every automatic trip (reconciliation drift, busted fill, daily loss, ...) schedules a
   * cancel-all; this carries it out. Call it regularly from the engine thread (it is also called
   * by submit() and reconcile()). It talks to the venue, so NEVER call it from the push thread:
   * that is the thread that delivers the venue's replies.
   */
  HaltReport serviceHalt();
  bool haltPending() const;

  // Broker events (pushes). Idempotent and order-independent.
  void onOrderUpdate(const opend::BrokerOrder& broker);
  void onFill(const opend::BrokerFill& fill);
  void onMark(const std::string& symbol, Money priceMills);

  /** Compares our view with the broker's; heals what is understood, halts on the rest. */
  ReconcileReport reconcile();
  /**
   * Called with NO lock held, after the order passed risk and before it is sent. Returning an
   * error means the intent could not be made durable: the order is NOT sent, the submit reports
   * kNotDurable and trading halts (a log that cannot be written cannot protect a restart).
   */
  void setDurableSubmitSink(std::function<Result<bool>(const DurableSubmit&)> sink);
  /**
   * Call after a restart and BEFORE bootstrap(): re-creates each logged intent as an order of
   * unknown outcome under its original ClOrdId and intent key. A reconciliation then finds it at
   * the broker (it carries the ClOrdId in its remark) or, after the grace period and several clean
   * listings, declares it dead; until then its key stays blocked, so a lost reply cannot be
   * followed by a second copy of the order. Entries older than `notBeforeNs` (earlier days, which
   * the broker no longer lists) are skipped. Returns how many were restored.
   */
  std::size_t restoreIntents(const std::vector<DurableSubmit>& intents, std::int64_t notBeforeNs);
  const OmsStats& stats() const;
  /** Marks "now" as the start of the trading day for daily-loss accounting. */
  void resetDailyBaseline();

  std::optional<OrderRecord> order(const std::string& clOrdId) const;
  std::vector<OrderRecord> orders() const;
  std::size_t liveOrderCount() const;
  /** True if any order in `symbol` is live. O(live orders), allocation-free: safe per quote. */
  bool hasLiveOrder(const std::string& symbol) const;
  /** ClOrdIds of live orders in `symbol`, in submission order (cheaper than copying orders()). */
  std::vector<std::string> liveOrderIds(const std::string& symbol) const;
  std::size_t unresolvedCount() const;
  std::size_t anomalyCount() const;
  std::vector<JournalEntry> journal() const;
  /** Called under the OMS lock for every journal entry: keep it fast (queue and return). */
  void setJournalSink(std::function<void(const JournalEntry&)> sink);

 private:
  SubmitResult submitInner(const OrderIntent& intent, const QuoteContext& callerQuote);

  struct Impl;
  std::unique_ptr<Impl> impl_;  // keeps the private state out of the public header
};

}  // namespace futu_trader::oms
