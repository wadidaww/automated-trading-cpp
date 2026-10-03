#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace futu_trader::opend {

using Sha1Digest = std::array<std::uint8_t, 20>;

/**
 * SHA-1 as required by the OpenD frame header (integrity check only, not security). Uses
 * OpenSSL, which is hardware accelerated: the portable version below spent ~1.4 us on a 160-byte
 * frame, over 90% of the whole decode.
 */
Sha1Digest sha1(const std::uint8_t* data, std::size_t size);

/**
 * Self-contained reference implementation. Not used on the hot path; it exists so tests can
 * cross-check the accelerated one (differential testing) and as documentation of the algorithm.
 */
Sha1Digest sha1Portable(const std::uint8_t* data, std::size_t size);

}  // namespace futu_trader::opend
