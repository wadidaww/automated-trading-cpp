---
name: futu-opend-integration
description: "Reference for integrating with Futu/moomoo OpenD: framing, connection lifecycle, TrdEnv/TrdMarket, unlock, pushes, quotas and gotchas. Use when touching src/opend or src/venue."
---

# Futu OpenD integration

OpenD is a local gateway speaking TCP + protobuf. Path: strategy -> loopback -> OpenD -> Futu servers -> exchange. No colocation; expect tens of ms.

## Verify, don't recall
Message layouts come from the vendored `.proto` files (`third_party/futu_proto`, pinned in `PROTO_VERSION`). Command IDs are **not** in the protos: they live in the SDK constant table and are pinned in `include/futu_trader/opend/proto_ids.hpp`, with a test asserting each value. Never copy an ID from memory into code. The legacy scaffold IDs were wrong (real: PlaceOrder 2202, GetOrderList 2201, ModifyOrder 2205, GetKL 3006, RequestHistoryKL 3103, GetSecuritySnapshot 3203). When upgrading OpenD, re-diff the SDK table and protos together.

Implementation map: `src/opend/{sha1,framing,connection,client}.cpp`; test fake: `tools/mock_opend`. Futu sends prices/qty/cash as doubles: convert with `opend::toMinor`/`toQuantity` (checked, rounds, rejects NaN/inf/fractions).

## Frame
44-byte header: "FT" magic, ProtoID, format type, version, serial number, body length, SHA1 of body, reserved. Little-endian. The SHA1 is an integrity check, not security. Reassemble streams; cap body length.

## Lifecycle
InitConnect -> (optional encryption) -> KeepAlive at server interval -> reconnect with jittered backoff -> resubscribe -> reconcile before trading. Trading needs `Trd_UnlockTrade` per session for REAL; password from secret store only.

## Trading semantics
- `TrdEnv` SIMULATE vs REAL is per request; account list (`Trd_GetAccList`) shows which accounts exist per env and market. Stamp env in the venue, assert it equals configured env.
- Cancel is a modify with an operation code. Correlate orders via the `remark` field carrying our ClOrdId.
- Subscribe to order/fill pushes; still reconcile via OrderList/FillList/PositionList/Funds.

## Limits (confirm for pinned version)
About 15 orders per 30 s per account; per-stock open order cap; subscription quota; minimum hold before unsubscribe; history-KL quota. Pushes are conflated snapshots, not a sequenced tick feed.

## Testing
Use `tools/mock_opend` with fault injection for CI. Real OpenD checks are manual or on a self-hosted runner, never on fork PRs.
