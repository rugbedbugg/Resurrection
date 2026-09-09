#include <cassert>
#include <cstdio>

#include "adaptive_restore/checkpoint/regions.hpp"
#include "adaptive_restore/common/version.hpp"

using namespace adaptive_restore;

int main() {
    constexpr std::uint64_t kR = kDefaultRegionBytes;
    assert(kR == 256 * 1024);

    // Two full regions plus a short tail.
    image::MappingRecord m{};
    m.mapping_id = 3;
    m.start_vaddr = 0x10000;
    m.end_vaddr = 0x10000 + kR * 2 + 4096;
    auto rs = checkpoint::subdivide(m, 10);
    assert(rs.size() == 3);
    assert(rs[0].region_id == 10 && rs[0].mapping_id == 3);
    assert(rs[0].start_vaddr == 0x10000);
    assert(rs[1].region_id == 11 && rs[1].start_vaddr == 0x10000 + kR);
    assert(rs[2].region_id == 12);
    assert(rs[2].end_vaddr == m.end_vaddr);
    assert(rs[2].end_vaddr - rs[2].start_vaddr == 4096);

    // Regions tile the mapping with no gap and no overlap.
    for (std::size_t i = 0; i + 1 < rs.size(); ++i)
        assert(rs[i].end_vaddr == rs[i + 1].start_vaddr);
    assert(rs.front().start_vaddr == m.start_vaddr);
    assert(rs.back().end_vaddr == m.end_vaddr);
    for (const auto& r : rs) assert(r.page_index_lo == 0 && r.page_index_hi == 0);

    // Exactly one region when the mapping is exactly one region long.
    image::MappingRecord exact{};
    exact.start_vaddr = 0x20000;
    exact.end_vaddr = 0x20000 + kR;
    auto one = checkpoint::subdivide(exact, 0);
    assert(one.size() == 1 && one[0].end_vaddr == exact.end_vaddr);

    // A single page still yields one region.
    image::MappingRecord tiny{};
    tiny.start_vaddr = 0x30000;
    tiny.end_vaddr = 0x31000;
    auto small = checkpoint::subdivide(tiny, 7);
    assert(small.size() == 1 && small[0].region_id == 7);
    assert(small[0].end_vaddr - small[0].start_vaddr == 4096);

    // An empty or inverted mapping yields nothing rather than looping.
    image::MappingRecord empty{};
    empty.start_vaddr = 0x40000;
    empty.end_vaddr = 0x40000;
    assert(checkpoint::subdivide(empty, 0).empty());
    image::MappingRecord inverted{};
    inverted.start_vaddr = 0x50000;
    inverted.end_vaddr = 0x40000;
    assert(checkpoint::subdivide(inverted, 0).empty());

    // A mapping at the top of the address space must not overflow.
    image::MappingRecord high{};
    high.start_vaddr = 0xffffffffff600000ull;
    high.end_vaddr = 0xffffffffff601000ull;
    auto top = checkpoint::subdivide(high, 0);
    assert(top.size() == 1 && top[0].end_vaddr == high.end_vaddr);

    std::puts("test_regions: ok");
    return 0;
}
