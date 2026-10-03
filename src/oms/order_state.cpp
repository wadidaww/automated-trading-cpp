#include "futu_trader/oms/order_state.hpp"

namespace futu_trader::oms {

std::optional<OmsState> transition(OmsState from, OmsEvent event) {
  if (isTerminal(from)) {
    return std::nullopt;
  }
  switch (event) {
    case OmsEvent::kSubmitSent:
      if (from == OmsState::kNew) {
        return OmsState::kPendingSubmit;
      }
      break;
    case OmsEvent::kAcked:
      if (from == OmsState::kPendingSubmit || from == OmsState::kUnknown) {
        return OmsState::kWorking;
      }
      break;
    case OmsEvent::kRejected:
      if (from == OmsState::kPendingSubmit || from == OmsState::kUnknown ||
          from == OmsState::kWorking) {
        return OmsState::kRejected;
      }
      break;
    case OmsEvent::kPartialFill:
      if (from != OmsState::kNew) {
        return OmsState::kPartiallyFilled;
      }
      break;
    case OmsEvent::kFullFill:
      if (from != OmsState::kNew) {
        return OmsState::kFilled;
      }
      break;
    case OmsEvent::kCancelRequested:
      if (from == OmsState::kWorking || from == OmsState::kPartiallyFilled ||
          from == OmsState::kPendingSubmit || from == OmsState::kUnknown) {
        return OmsState::kCancelPending;
      }
      break;
    case OmsEvent::kCancelConfirmed:
      if (from != OmsState::kNew) {
        return OmsState::kCancelled;
      }
      break;
    case OmsEvent::kCancelRejected:
      if (from == OmsState::kCancelPending) {
        return OmsState::kWorking;
      }
      break;
    case OmsEvent::kSubmitAmbiguous:
      if (from == OmsState::kPendingSubmit) {
        return OmsState::kUnknown;
      }
      break;
  }
  return std::nullopt;
}

const char* toString(OmsState state) {
  switch (state) {
    case OmsState::kNew:
      return "new";
    case OmsState::kPendingSubmit:
      return "pending_submit";
    case OmsState::kWorking:
      return "working";
    case OmsState::kPartiallyFilled:
      return "partially_filled";
    case OmsState::kCancelPending:
      return "cancel_pending";
    case OmsState::kFilled:
      return "filled";
    case OmsState::kCancelled:
      return "cancelled";
    case OmsState::kRejected:
      return "rejected";
    case OmsState::kUnknown:
      return "unknown";
  }
  return "invalid";
}

std::optional<OmsState> fromFutuStatus(std::int32_t futuStatus) {
  switch (futuStatus) {
    case 0:  // Unsubmitted
    case 1:  // WaitingSubmit
    case 2:  // Submitting
      return OmsState::kPendingSubmit;
    case 3:   // SubmitFailed
    case 21:  // Failed
      return OmsState::kRejected;
    case 4:   // TimeOut: submission outcome unclear
    case -1:  // Unknown
    case 24:  // FillCancelled: a fill was busted; needs human/reconciler attention
      return OmsState::kUnknown;
    case 5:  // Submitted
      return OmsState::kWorking;
    case 10:  // Filled_Part
      return OmsState::kPartiallyFilled;
    case 11:  // Filled_All
      return OmsState::kFilled;
    case 12:  // Cancelling_Part
    case 13:  // Cancelling_All
      return OmsState::kCancelPending;
    case 14:  // Cancelled_Part
    case 15:  // Cancelled_All
    case 22:  // Disabled
    case 23:  // Deleted
      return OmsState::kCancelled;
    default:
      return std::nullopt;
  }
}

}  // namespace futu_trader::oms
