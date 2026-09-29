---
name: hk-market-rules
description: "HKEX rules for the trading system: board lots, tick sizes, sessions, auctions, fees and stamp duty, short-selling."
---

# HKEX rules (verify against current HKEX/broker schedules before encoding)

- Sessions: pre-opening auction, morning continuous, lunch break, afternoon continuous, closing auction; verify current hours and any holiday half-days.
- Tick size depends on price band; lot size is per security (board lot). Put both in the instrument table, never in code constants.
- Costs: commission, platform fee, trading fee, SFC transaction levy, settlement fee, stamp duty (charged on both sides, rounded up to the nearest HKD). Rates change; keep them in versioned config with an effective date.
- Short selling only for designated securities; check the designated list and Futu availability.
- Halts and suspensions must be handled by SimVenue and live risk.
- Encode each rule with a golden test computed by hand.
