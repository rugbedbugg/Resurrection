#include "adaptive_restore/common/crc32c.hpp"

#if defined(__x86_64__)
#  include <nmmintrin.h>   // _mm_crc32_u* (SSE4.2)
#  include <cpuid.h>
#endif

namespace adaptive_restore {
namespace {

std::uint32_t g_table[256];
bool g_table_ready = [] {
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        g_table[i] = c;
    }
    return true;
}();

bool have_sse42() {
#if defined(__x86_64__)
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
    return (c & bit_SSE4_2) != 0;
#else
    return false;
#endif
}

std::uint32_t crc32c_scalar(const std::byte* p, std::size_t n, std::uint32_t crc) {
    for (std::size_t i = 0; i < n; ++i)
        crc = g_table[(crc ^ std::to_integer<std::uint8_t>(p[i])) & 0xFF] ^ (crc >> 8);
    return crc;
}

#if defined(__x86_64__)
__attribute__((target("sse4.2")))
std::uint32_t crc32c_hw(const std::byte* p, std::size_t n, std::uint32_t crc) {
    while (n >= 8) {
        std::uint64_t v;
        __builtin_memcpy(&v, p, 8);
        crc = static_cast<std::uint32_t>(_mm_crc32_u64(crc, v));
        p += 8; n -= 8;
    }
    while (n--) crc = _mm_crc32_u8(crc, std::to_integer<std::uint8_t>(*p++));
    return crc;
}
#endif

}  // namespace

std::uint32_t crc32c(std::span<const std::byte> data, std::uint32_t seed) {
    (void)g_table_ready;
    std::uint32_t crc = ~seed;
#if defined(__x86_64__)
    static const bool hw = have_sse42();
    crc = hw ? crc32c_hw(data.data(), data.size(), crc)
             : crc32c_scalar(data.data(), data.size(), crc);
#else
    crc = crc32c_scalar(data.data(), data.size(), crc);
#endif
    return ~crc;
}

}  // namespace adaptive_restore
