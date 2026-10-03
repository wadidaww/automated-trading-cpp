#pragma once

namespace futu_trader::infra {

/**
 * Best-effort Linux thread tuning for the latency-critical threads. Each returns whether it
 * succeeded and never throws: lacking CAP_SYS_NICE or running in a container/WSL is normal, and
 * the system must still run (just with more jitter), so callers log the result, not abort.
 *
 * Pin the engine and market-data threads to cores isolated from the scheduler (`isolcpus=`,
 * `nohz_full=`, IRQs steered away) for the lowest jitter; pinning alone to a shared core helps
 * little.
 */
bool pinCurrentThreadToCpu(int cpu);
/** SCHED_FIFO at `priority` (1..99). Needs CAP_SYS_NICE; a runaway busy-poll can starve the host.
 */
bool setCurrentThreadRealtime(int priority);

}  // namespace futu_trader::infra
