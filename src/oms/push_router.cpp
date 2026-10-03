#include "futu_trader/oms/push_router.hpp"

#include "futu_trader/opend/proto_ids.hpp"
#include "futu_trader/opend/trade_decode.hpp"

namespace futu_trader::oms {

bool PushRouter::ours(const opend::WireHeader& header) const {
  return header.env == target_.env() && header.accId == target_.accId() &&
         header.market == target_.market();
}

void PushRouter::onFrame(const opend::Frame& frame) {
  if (frame.protoId == opend::protoId::kTrdUpdateOrder) {
    const auto update = opend::decodeOrderUpdate(frame);
    if (!update) {
      ++undecodable_;
    } else if (!ours(update.value().header)) {
      ++foreign_;
    } else {
      oms_.onOrderUpdate(update.value().order);
      ++routed_;
    }
  } else if (frame.protoId == opend::protoId::kTrdUpdateOrderFill) {
    const auto update = opend::decodeFillUpdate(frame);
    if (!update) {
      ++undecodable_;
    } else if (!ours(update.value().header)) {
      ++foreign_;
    } else {
      oms_.onFill(update.value().fill);
      ++routed_;
    }
  }
}

}  // namespace futu_trader::oms
