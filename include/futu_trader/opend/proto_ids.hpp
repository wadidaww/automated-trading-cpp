#pragma once

#include <cstdint>

/**
 * OpenD command IDs. The `.proto` files do not carry these, so they are pinned to the official
 * SDK constant table (futu-api 10.11.7108, futu/common/constant.py `ProtoId`); see
 * third_party/futu_proto/PROTO_VERSION. tests/unit/opend_framing_test.cpp asserts every value.
 * When upgrading OpenD, re-diff against the new SDK before changing anything here.
 */
namespace futu_trader::opend::protoId {
inline constexpr std::uint32_t kInitConnect = 1001;
inline constexpr std::uint32_t kGetGlobalState = 1002;
inline constexpr std::uint32_t kNotify = 1003;
inline constexpr std::uint32_t kKeepAlive = 1004;

inline constexpr std::uint32_t kTrdGetAccList = 2001;
inline constexpr std::uint32_t kTrdUnlockTrade = 2005;
inline constexpr std::uint32_t kTrdSubAccPush = 2008;
inline constexpr std::uint32_t kTrdGetFunds = 2101;
inline constexpr std::uint32_t kTrdGetPositionList = 2102;
inline constexpr std::uint32_t kTrdGetOrderList = 2201;
inline constexpr std::uint32_t kTrdPlaceOrder = 2202;
inline constexpr std::uint32_t kTrdModifyOrder = 2205;  // cancel is a modify with an op code
inline constexpr std::uint32_t kTrdUpdateOrder = 2208;  // push
inline constexpr std::uint32_t kTrdGetOrderFillList = 2211;
inline constexpr std::uint32_t kTrdUpdateOrderFill = 2218;  // push

inline constexpr std::uint32_t kQotSub = 3001;
inline constexpr std::uint32_t kQotGetBasicQot = 3004;
inline constexpr std::uint32_t kQotUpdateBasicQot = 3005;  // push
inline constexpr std::uint32_t kQotGetKL = 3006;
inline constexpr std::uint32_t kQotUpdateKL = 3007;         // push
inline constexpr std::uint32_t kQotUpdateTicker = 3011;     // push
inline constexpr std::uint32_t kQotUpdateOrderBook = 3013;  // push
inline constexpr std::uint32_t kQotRequestHistoryKL = 3103;
}  // namespace futu_trader::opend::protoId
