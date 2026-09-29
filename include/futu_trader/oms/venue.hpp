#pragma once

#include <cstdint>
#include <vector>

#include "futu_trader/core/result.hpp"
#include "futu_trader/opend/client.hpp"
#include "futu_trader/opend/types.hpp"

namespace futu_trader::oms {

/**
 * What the OMS needs from a broker. Error codes carry meaning the OMS relies on:
 *   kServer / kInvalidArg         -> the broker definitively refused: the order does not exist
 *   kTimeout / kDisconnected /
 *   kProtocol                     -> AMBIGUOUS: the order may or may not exist
 */
class IVenue {
 public:
  virtual ~IVenue() = default;
  virtual Result<opend::PlacedOrder> place(const opend::PlaceOrderRequest& request) = 0;
  virtual Result<bool> cancel(std::uint64_t venueOrderId) = 0;
  virtual Result<std::vector<opend::BrokerOrder>> listOrders() = 0;
  virtual Result<std::vector<opend::BrokerFill>> listFills() = 0;
  virtual Result<std::vector<opend::PositionInfo>> listPositions() = 0;
  virtual Result<opend::FundsInfo> funds() = 0;
};

}  // namespace futu_trader::oms
