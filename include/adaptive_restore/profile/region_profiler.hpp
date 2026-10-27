#pragma once
#include <cstdint>
#include <vector>

#include "adaptive_restore/paging/lazy_provider.hpp"
#include "adaptive_restore/paging/page_provider.hpp"
#include "adaptive_restore/profile/profile_store.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::profile {

// What one restore actually witnessed about one region.
//
// Two masks, deliberately not one. `touched` says a page faulted, which is
// positive evidence that the workload wanted it. `observable` says the page
// *could* have faulted, i.e. it was absent at resume. A page that is not
// observable is UNKNOWN: it was resident, nothing watches reads of a resident
// page, and recording a zero for it would teach "never accessed" about exactly
// the pages prefetch predicted correctly.
//
// Absence of a fault is only evidence where `observable` is set.
struct RegionObservation {
    std::uint32_t region_id = 0;
    std::uint32_t pages = 0;          // pages in the region
    std::uint32_t zero_pages = 0;     // zero-encoded, from the image
    std::vector<std::uint8_t> touched;
    std::vector<std::uint8_t> observable;

    bool bit(const std::vector<std::uint8_t>& m, std::uint32_t i) const {
        const std::size_t byte = i / 8;
        return byte < m.size() && (m[byte] & (1u << (i % 8)));
    }
    std::uint32_t observable_count() const {
        std::uint32_t n = 0;
        for (std::uint32_t i = 0; i < pages; ++i) n += bit(observable, i);
        return n;
    }
    std::uint32_t touched_count() const {
        std::uint32_t n = 0;
        for (std::uint32_t i = 0; i < pages; ++i) n += bit(touched, i);
        return n;
    }
    // Complete evidence: every page of the region could have faulted, so a
    // clear bit in `touched` genuinely means untouched.
    bool fully_observable() const { return observable_count() == pages && pages > 0; }
};

// Turn one restore's demand-fault trace into per-region observations.
//
// `execution` is indexed by region id and says what the provider ACTUALLY did,
// which is not always what the planner requested: a StaticPrefetch region with
// no usable history writes nothing and is fully observable. Keying off the
// requested action instead discarded those observations entirely -- thousands
// of real faults per run -- which is the bug this signature exists to prevent.
std::vector<RegionObservation> observe_regions(
    const image::ImageReader& image, const std::vector<paging::FaultRecord>& trace,
    const std::vector<paging::RegionExecution>& execution);

// Narrow observations down to the ones the v2 profile schema can hold without
// inventing evidence: those with complete coverage.
//
// A partially observed region carries real information (its misses prove the
// prediction is incomplete), but v2's single `touched_bitmap` has no way to say
// "this bit is unknown". Merging it would silently fabricate negatives, so
// Phase 6A reports those regions and stores none of them. Phase 6B's schema is
// where partial evidence becomes persistable.
std::vector<RegionHistory> to_history(const std::vector<RegionObservation>& observed,
                                      std::uint32_t* skipped_partial = nullptr);

}  // namespace adaptive_restore::profile
