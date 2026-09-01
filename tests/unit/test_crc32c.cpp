#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include "adaptive_restore/common/crc32c.hpp"

static std::span<const std::byte> s(const char* p) {
    return {reinterpret_cast<const std::byte*>(p), std::strlen(p)};
}

int main() {
    // Canonical CRC-32C (Castagnoli) test vectors.
    assert(adaptive_restore::crc32c(s("")) == 0x00000000u);
    assert(adaptive_restore::crc32c(s("123456789")) == 0xE3069283u);

    // Streaming must equal one-shot.
    auto full = s("hello world");
    std::uint32_t part = adaptive_restore::crc32c(full.subspan(0, 5));
    part = adaptive_restore::crc32c(full.subspan(5), part);
    assert(part == adaptive_restore::crc32c(full));

    std::puts("test_crc32c: ok");
    return 0;
}
