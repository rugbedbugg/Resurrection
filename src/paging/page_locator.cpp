#include "adaptive_restore/paging/page_locator.hpp"

#include <algorithm>

#include "adaptive_restore/image/image_reader.hpp"

namespace adaptive_restore::paging {

PageLocator::PageLocator(const image::ImageReader& image)
    : pages_(image.page_index()),
      regions_(image.regions()),
      page_size_(image.header().page_size) {
    // The format says the page index is sorted by vaddr; the binary search
    // below is wrong if it is not, so make that an enforced precondition
    // rather than a hope.
    std::sort(pages_.begin(), pages_.end(),
              [](const image::PageIndexRecord& a, const image::PageIndexRecord& b) {
                  return a.vaddr < b.vaddr;
              });
}

std::size_t PageLocator::find(std::uint64_t vaddr) const {
    const auto it = std::upper_bound(pages_.begin(), pages_.end(), vaddr,
                                     [](std::uint64_t v, const image::PageIndexRecord& p) {
                                         return v < p.vaddr;
                                     });
    if (it == pages_.begin()) return npos;
    const auto prev = std::prev(it);
    if (vaddr >= prev->vaddr && vaddr < prev->vaddr + page_size_)
        return static_cast<std::size_t>(prev - pages_.begin());
    return npos;
}

std::uint32_t PageLocator::region_of(std::size_t page) const {
    for (const auto& r : regions_)
        if (page >= r.page_index_lo && page < r.page_index_hi) return r.region_id;
    return static_cast<std::uint32_t>(npos);
}

}  // namespace adaptive_restore::paging
