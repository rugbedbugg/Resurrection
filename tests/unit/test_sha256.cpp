#include <cassert>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include "adaptive_restore/common/sha256.hpp"

static std::string hex(std::span<const std::byte, 32> d) {
    static const char* k = "0123456789abcdef";
    std::string o;
    for (std::byte b : d) {
        auto v = std::to_integer<unsigned>(b);
        o += k[v >> 4]; o += k[v & 15];
    }
    return o;
}

int main() {
    auto empty = adaptive_restore::sha256({});
    assert(hex(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const char* abc = "abc";
    auto d = adaptive_restore::sha256({reinterpret_cast<const std::byte*>(abc), 3});
    assert(hex(d) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    std::puts("test_sha256: ok");
    return 0;
}
