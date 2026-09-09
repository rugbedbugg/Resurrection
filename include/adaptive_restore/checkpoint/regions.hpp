#pragma once
#include <cstdint>
#include <vector>

#include "adaptive_restore/image/format.hpp"

namespace adaptive_restore::checkpoint {

// Cut a saved mapping into fixed kDefaultRegionBytes (256 KiB) slices, the unit
// the policy engine makes its eager/lazy/prefetch decision over. The final slice
// is clamped to the mapping end and may be short.
//
// page_index_lo/hi are left zero here and filled in by the capture pass once the
// pages have been indexed. Region ids run first_region_id, +1, ...
std::vector<image::RegionRecord> subdivide(const image::MappingRecord& m,
                                           std::uint32_t first_region_id);

}  // namespace adaptive_restore::checkpoint
