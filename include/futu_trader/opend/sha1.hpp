#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace futu_trader::opend {

using Sha1Digest = std::array<std::uint8_t, 20>;

/** SHA-1 as required by the OpenD frame header. Integrity check only, not security. */
Sha1Digest sha1(const std::uint8_t* data, std::size_t size);

}  // namespace futu_trader::opend
