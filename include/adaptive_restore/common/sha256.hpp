#pragma once
#include <array>
#include <cstddef>
#include <span>

namespace adaptive_restore {

// One-shot SHA-256. Used only for the image header's launch_env_digest.
std::array<std::byte, 32> sha256(std::span<const std::byte> data);

}  // namespace adaptive_restore
