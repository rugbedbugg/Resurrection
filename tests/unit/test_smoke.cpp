// Smoke test: the tree compiles, headers are reachable, invariants hold.
// Replace/expand once Phase 1 adds real image encode/decode logic (§25.1).

#include <cstdio>

#include "adaptive_restore/common/version.hpp"

int main() {
    using namespace adaptive_restore;

    static_assert(kImageFormatVersion == 1);
    static_assert(kDefaultRegionBytes % 4096 == 0, "region size must be page-aligned");
    static_assert(sizeof(kImageMagic) == 8, "image magic is 8 bytes (§7.2)");

    if (kProjectVersion[0] == '\0') { std::puts("empty version string"); return 1; }

    std::puts("smoke: ok");
    return 0;
}
