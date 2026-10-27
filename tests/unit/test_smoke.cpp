// Smoke test: the tree compiles, headers are reachable, invariants hold.

#include <cstdio>

#include "adaptive_restore/common/version.hpp"

int main() {
    using namespace adaptive_restore;

    // Canary: bump deliberately when the on-disk format changes.
    // v2 added PageEncoding::FromFile (file-page referencing).
    static_assert(kImageFormatVersion == 2);
    static_assert(kDefaultRegionBytes % 4096 == 0, "region size must be page-aligned");
    static_assert(sizeof(kImageMagic) == 8, "image magic is 8 bytes");

    if (kProjectVersion[0] == '\0') { std::puts("empty version string"); return 1; }

    std::puts("smoke: ok");
    return 0;
}
