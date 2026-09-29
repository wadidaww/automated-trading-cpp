#pragma once

#include "futu_trader/oms/live_gate.hpp"
#include "futu_trader/oms/venue.hpp"

namespace futu_trader::oms {

/**
 * IVenue backed by OpenD. The TradeTarget (SIMULATE vs REAL, account, market) is fixed at
 * construction and stamped into every request here, and nowhere else: callers cannot pass an
 * environment per order, and a REAL target cannot exist without a LiveApproval.
 */
class OpenDVenue final : public IVenue {
 public:
  OpenDVenue(opend::OpenDClient& client, TradeTarget target)
      : client_(client), target_(target), header_(target.header()) {}

  const TradeTarget& target() const { return target_; }

  Result<opend::PlacedOrder> place(const opend::PlaceOrderRequest& request) override {
    return client_.placeOrder(header_, request);
  }
  Result<bool> cancel(std::uint64_t venueOrderId) override {
    return client_.cancelOrder(header_, venueOrderId);
  }
  Result<std::vector<opend::BrokerOrder>> listOrders() override {
    return client_.getOrderList(header_);
  }
  Result<std::vector<opend::BrokerFill>> listFills() override {
    return client_.getOrderFillList(header_);
  }
  Result<std::vector<opend::PositionInfo>> listPositions() override {
    return client_.getPositions(header_);
  }
  Result<opend::FundsInfo> funds() override { return client_.getFunds(header_); }

 private:
  opend::OpenDClient& client_;
  TradeTarget target_;
  opend::AccountHeader header_;
};

}  // namespace futu_trader::oms
