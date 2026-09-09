#include "adaptive_restore/checkpoint/regions.hpp"

#include "adaptive_restore/common/version.hpp"

namespace adaptive_restore::checkpoint {

std::vector<image::RegionRecord> subdivide(const image::MappingRecord& m,
                                           std::uint32_t first_region_id) {
    std::vector<image::RegionRecord> out;
    if (m.end_vaddr <= m.start_vaddr) return out;

    constexpr std::uint64_t kStep = kDefaultRegionBytes;
    const std::uint64_t total = m.end_vaddr - m.start_vaddr;
    out.reserve(static_cast<std::size_t>((total + kStep - 1) / kStep));

    std::uint32_t id = first_region_id;
    for (std::uint64_t addr = m.start_vaddr; addr < m.end_vaddr;) {
        // Subtract rather than add: a mapping at the top of the address space
        // would overflow addr + kStep.
        const std::uint64_t remaining = m.end_vaddr - addr;
        const std::uint64_t span = (remaining < kStep) ? remaining : kStep;

        image::RegionRecord r;
        r.region_id = id++;
        r.mapping_id = m.mapping_id;
        r.start_vaddr = addr;
        r.end_vaddr = addr + span;
        out.push_back(r);

        addr += span;
    }
    return out;
}

}  // namespace adaptive_restore::checkpoint
