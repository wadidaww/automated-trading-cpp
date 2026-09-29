#include "futu_trader/oms/order_state.hpp"

#include <gtest/gtest.h>

#include <random>

using namespace futu_trader::oms;

namespace {
constexpr OmsState kAllStates[] = {OmsState::kNew,           OmsState::kPendingSubmit,
                                   OmsState::kWorking,       OmsState::kPartiallyFilled,
                                   OmsState::kCancelPending, OmsState::kFilled,
                                   OmsState::kCancelled,     OmsState::kRejected,
                                   OmsState::kUnknown};
constexpr OmsEvent kAllEvents[] = {
    OmsEvent::kSubmitSent,      OmsEvent::kAcked,          OmsEvent::kRejected,
    OmsEvent::kPartialFill,     OmsEvent::kFullFill,       OmsEvent::kCancelRequested,
    OmsEvent::kCancelConfirmed, OmsEvent::kCancelRejected, OmsEvent::kSubmitAmbiguous};
}  // namespace

TEST(OrderFsm, HappyPathToFilled) {
  auto s = transition(OmsState::kNew, OmsEvent::kSubmitSent);
  ASSERT_EQ(s, OmsState::kPendingSubmit);
  s = transition(*s, OmsEvent::kAcked);
  ASSERT_EQ(s, OmsState::kWorking);
  s = transition(*s, OmsEvent::kPartialFill);
  ASSERT_EQ(s, OmsState::kPartiallyFilled);
  s = transition(*s, OmsEvent::kFullFill);
  EXPECT_EQ(s, OmsState::kFilled);
}

TEST(OrderFsm, CancelFlowAndCancelRejected) {
  auto s = transition(OmsState::kWorking, OmsEvent::kCancelRequested);
  ASSERT_EQ(s, OmsState::kCancelPending);
  EXPECT_EQ(transition(*s, OmsEvent::kCancelRejected), OmsState::kWorking);
  EXPECT_EQ(transition(*s, OmsEvent::kCancelConfirmed), OmsState::kCancelled);
  // A fill can race the cancel.
  EXPECT_EQ(transition(*s, OmsEvent::kFullFill), OmsState::kFilled);
}

TEST(OrderFsm, AmbiguousSubmitResolvesEitherWay) {
  const auto unknown = transition(OmsState::kPendingSubmit, OmsEvent::kSubmitAmbiguous);
  ASSERT_EQ(unknown, OmsState::kUnknown);
  EXPECT_EQ(transition(*unknown, OmsEvent::kAcked), OmsState::kWorking);      // it did exist
  EXPECT_EQ(transition(*unknown, OmsEvent::kRejected), OmsState::kRejected);  // it did not
  EXPECT_EQ(transition(*unknown, OmsEvent::kFullFill), OmsState::kFilled);    // filled unseen
}

TEST(OrderFsm, TerminalStatesAreFinalForEveryEvent) {
  for (OmsState terminal : {OmsState::kFilled, OmsState::kCancelled, OmsState::kRejected}) {
    for (OmsEvent event : kAllEvents) {
      EXPECT_FALSE(transition(terminal, event).has_value())
          << toString(terminal) << " accepted an event";
    }
  }
}

TEST(OrderFsm, NewOrderOnlyAcceptsSubmitSent) {
  for (OmsEvent event : kAllEvents) {
    const auto next = transition(OmsState::kNew, event);
    EXPECT_EQ(next.has_value(), event == OmsEvent::kSubmitSent);
  }
}

TEST(OrderFsm, RandomEventSequencesNeverEscapeTerminalStates) {
  // Property: whatever illegal/duplicate events arrive, once terminal always terminal, and the
  // machine never produces a state outside the enum. Illegal events must simply be refused.
  std::mt19937 rng(99);
  for (int round = 0; round < 5000; ++round) {
    OmsState state = OmsState::kNew;
    bool reachedTerminal = false;
    for (int step = 0; step < 30; ++step) {
      const OmsEvent event = kAllEvents[rng() % std::size(kAllEvents)];
      const auto next = transition(state, event);
      if (reachedTerminal) {
        ASSERT_FALSE(next.has_value());
        continue;
      }
      if (next) {
        state = *next;
        reachedTerminal = isTerminal(state);
      }
    }
  }
}

TEST(OrderFsm, LivenessClassification) {
  EXPECT_FALSE(isLive(OmsState::kNew));
  EXPECT_TRUE(isLive(OmsState::kPendingSubmit));
  EXPECT_TRUE(isLive(OmsState::kUnknown));  // may exist at the broker: must count against risk
  EXPECT_FALSE(isLive(OmsState::kFilled));
}

TEST(OrderFsm, FutuStatusMapping) {
  EXPECT_EQ(fromFutuStatus(5), OmsState::kWorking);
  EXPECT_EQ(fromFutuStatus(10), OmsState::kPartiallyFilled);
  EXPECT_EQ(fromFutuStatus(11), OmsState::kFilled);
  EXPECT_EQ(fromFutuStatus(15), OmsState::kCancelled);
  EXPECT_EQ(fromFutuStatus(21), OmsState::kRejected);
  EXPECT_EQ(fromFutuStatus(3), OmsState::kRejected);
  EXPECT_EQ(fromFutuStatus(4), OmsState::kUnknown);   // timeout is not a success
  EXPECT_EQ(fromFutuStatus(24), OmsState::kUnknown);  // busted fill needs attention
  EXPECT_FALSE(fromFutuStatus(9999).has_value());     // unrecognised must not be guessed
}
