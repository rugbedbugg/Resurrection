#pragma once
#include <cstdint>
#include <vector>

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::paging {

struct LazyRange {
    std::uint64_t start = 0, end = 0;
    std::uint32_t mapping_id = 0;
};

// Which of an image's mappings can be served by userfaultfd MISSING mode.
//
// Only private anonymous mappings whose contents were saved. File-backed
// mappings are excluded: MISSING mode does not apply to them, and they are
// already demand-paged by the kernel from their own file. Kernel-special and
// no-access mappings carry nothing to serve.
//
// Lives here rather than beside the Userfault class so that code which only
// reasons about a plan -- the policy layer, `arctl visualize` -- can ask the
// question without dragging in compel.
std::vector<LazyRange> lazy_ranges(const image::ImageReader& image);

}  // namespace adaptive_restore::paging
