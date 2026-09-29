#pragma once

#include <cstdint>
#include <optional>

namespace futu_trader::oms {

/**
 * Internal order lifecycle. kUnknown means the outcome of a submit is ambiguous (timeout or
 * disconnect after sending): the order may or may not exist at the broker, so the intent stays
 * blocked until reconciliation resolves it.
 */
enum class OmsState : std::uint8_t {
  kNew,
  kPendingSubmit,
  kWorking,
  kPartiallyFilled,
  kCancelPending,
  kFilled,
  kCancelled,
  kRejected,
  kUnknown,
};

enum class OmsEvent : std::uint8_t {
  kSubmitSent,
  kAcked,
  kRejected,
  kPartialFill,
  kFullFill,
  kCancelRequested,
  kCancelConfirmed,
  kCancelRejected,
  kSubmitAmbiguous,
};

constexpr bool isTerminal(OmsState state) {
  return state == OmsState::kFilled || state == OmsState::kCancelled ||
         state == OmsState::kRejected;
}

/** States in which the order can still trade at the broker (or might, if unknown). */
constexpr bool isLive(OmsState state) { return !isTerminal(state) && state != OmsState::kNew; }

/** Returns the next state, or nullopt if the event is illegal in `from`. Terminal states are final.
 */
std::optional<OmsState> transition(OmsState from, OmsEvent event);

const char* toString(OmsState state);

/**
 * Maps Futu's Trd_Common.OrderStatus to the state we would observe, or nullopt for values we do
 * not understand (which the OMS must treat as kUnknown, never as success).
 */
std::optional<OmsState> fromFutuStatus(std::int32_t futuStatus);

}  // namespace futu_trader::oms
