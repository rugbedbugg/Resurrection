#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace adaptive_restore {

// CRC-32C (Castagnoli, poly 0x1EDC6F41), reflected, init/xorout 0xFFFFFFFF.
// Pass the previous result as `seed` to continue a running CRC.
std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed = 0);

}  // namespace adaptive_restore
