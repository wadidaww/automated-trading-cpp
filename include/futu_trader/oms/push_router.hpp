#pragma once

#include <atomic>
#include <cstddef>

#include "futu_trader/oms/live_gate.hpp"
#include "futu_trader/oms/oms.hpp"
#include "futu_trader/opend/framing.hpp"

namespace futu_trader::oms {

/**
 * Decodes OpenD order/fill pushes and hands them to the OMS, but only if they belong to the
 * account, environment and market this OMS trades. A SIMULATE fill (or another account's fill)
 * reaching a REAL book would corrupt positions, so anything else is dropped and counted.
 *
 * Install as the OpenDClient push handler. It runs on the OpenD reader thread and therefore never
 * calls the venue; it only feeds Oms::onOrderUpdate / onFill, which just take a short lock.
 */
class PushRouter {
 public:
  PushRouter(Oms& oms, const TradeTarget& target) : oms_(oms), target_(target) {}

  void onFrame(const opend::Frame& frame);

  std::size_t routed() const { return routed_.load(); }
  /** Pushes for a different account/environment/market than ours. Should stay at zero. */
  std::size_t foreign() const { return foreign_.load(); }
  /** Pushes we could not decode or understand. */
  std::size_t undecodable() const { return undecodable_.load(); }

 private:
  bool ours(const opend::WireHeader& header) const;

  Oms& oms_;
  TradeTarget target_;
  std::atomic<std::size_t> routed_{0};
  std::atomic<std::size_t> foreign_{0};
  std::atomic<std::size_t> undecodable_{0};
};

}  // namespace futu_trader::oms
