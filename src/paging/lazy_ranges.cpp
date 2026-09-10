#include "adaptive_restore/paging/lazy_ranges.hpp"

#include "adaptive_restore/image/format.hpp"
#include "adaptive_restore/image/image_reader.hpp"

namespace adaptive_restore::paging {

std::vector<LazyRange> lazy_ranges(const image::ImageReader& image) {
    std::vector<LazyRange> out;
    for (const auto& m : image.mappings()) {
        if (!(m.flags & image::MappingFlag::ContentsSaved)) continue;
        if (m.flags & image::MappingFlag::KernelSpecial) continue;
        if (m.flags & image::MappingFlag::Shared) continue;   // MISSING mode is private-only
        if (m.flags & image::MappingFlag::FileBacked) continue;
        // NB: do NOT filter on a non-empty path here. [heap] and [stack] carry
        // their kernel tag in the path field while being ordinary private
        // anonymous memory, and excluding them leaves the largest lazy
        // candidates in the image being restored eagerly for no reason.
        out.push_back({m.start_vaddr, m.end_vaddr, m.mapping_id});
    }
    return out;
}

}  // namespace adaptive_restore::paging
