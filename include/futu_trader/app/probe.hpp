#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace futu_trader::app {

/**
 * Asks a local metrics endpoint for /readyz and says whether it answered 200. For container and
 * orchestrator health checks, where the image has no curl. Loopback only; bounded by `timeout`.
 */
bool probeReady(std::uint16_t port, std::chrono::milliseconds timeout);

}  // namespace futu_trader::app
