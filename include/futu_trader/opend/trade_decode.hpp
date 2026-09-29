#pragma once

#include "Trd_Common.pb.h"
#include "futu_trader/core/result.hpp"
#include "futu_trader/opend/framing.hpp"
#include "futu_trader/opend/types.hpp"

namespace futu_trader::opend {

/** Protobuf -> domain conversions (checked: NaN, overflow and fractional shares are errors). */
Result<BrokerOrder> convertOrder(const Trd_Common::Order& order);
Result<BrokerFill> convertFill(const Trd_Common::OrderFill& item);

/** Decode Trd_UpdateOrder (2208) / Trd_UpdateOrderFill (2218) push frames. */
Result<OrderUpdate> decodeOrderUpdate(const Frame& frame);
Result<FillUpdate> decodeFillUpdate(const Frame& frame);

}  // namespace futu_trader::opend
