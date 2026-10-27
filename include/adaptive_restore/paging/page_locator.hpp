#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "adaptive_restore/image/format.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::paging {

// Maps a faulting address to its page record and owning region.
//
// The page index is sorted by vaddr, so this is a binary search. A
// linear scan would be O(pages) per fault and would dominate the very latency
// the benchmark is trying to measure.
class PageLocator {
public:
    explicit PageLocator(const image::ImageReader& image);

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // Index of the page containing `vaddr`, or npos.
    std::size_t find(std::uint64_t vaddr) const;

    // Region owning a page index, or npos if no region claims it.
    std::uint32_t region_of(std::size_t page) const;

    const std::vector<image::PageIndexRecord>& pages() const { return pages_; }
    std::uint64_t page_size() const { return page_size_; }

private:
    std::vector<image::PageIndexRecord> pages_;
    std::vector<image::RegionRecord> regions_;
    std::uint64_t page_size_ = 4096;
};

}  // namespace adaptive_restore::paging
