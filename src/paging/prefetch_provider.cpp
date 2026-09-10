#include "adaptive_restore/paging/prefetch_provider.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/paging/transfer.hpp"

namespace adaptive_restore::paging {

PrefetchProvider::PrefetchProvider(const image::ImageReader& image,
                                   restore::RestoreTarget& target, restore::Userfault& uffd,
                                   const std::vector<profile::RegionHistory>& history)
    : image_(image), target_(target), lazy_(image, uffd) {
    for (const auto& h : history) history_.emplace(h.region_id, h);
}

void PrefetchProvider::prepare_region(std::uint32_t region_id, RestoreAction action) {
    if (action != RestoreAction::StaticPrefetch && action != RestoreAction::Lazy)
        throw CheckpointError("PrefetchProvider: asked to prepare region " +
                              std::to_string(region_id) + " with an unsupported action");

    // Register first, then populate. Registering a range whose pages are
    // already present would leave those pages silently un-handled, and any
    // later fault on them would resolve as a zero page.
    lazy_.prepare_region(region_id, RestoreAction::Lazy);

    // A Lazy region is a prefetch region with nothing predicted, which is the
    // same code path a region with no history already takes.
    if (action == RestoreAction::Lazy) return;

    const auto it = history_.find(region_id);
    if (it == history_.end() || it->second.touched_bitmap.empty()) {
        // No recorded working set: nothing to predict, so behave as pure lazy.
        ++stats_.regions_without_history;
        return;
    }

    const auto regions = image_.regions();
    const auto& r = regions[region_id];
    const auto& pages = image_.page_index();
    const std::uint64_t page_size = image_.header().page_size;
    const std::uint32_t n = r.page_index_hi - r.page_index_lo;

    bool any = false;
    for (std::uint32_t p = 0; p < n; ++p) {
        if (!it->second.page_was_touched(p)) continue;
        const std::uint32_t gi = r.page_index_lo + p;
        if (gi >= pages.size()) continue;
        const auto& rec = pages[gi];

        // A registered page must be filled through the uffd, not written behind
        // its back: process_vm_writev would resolve the fault the kernel is
        // still holding and the handler would never be told.
        PageRequest req;
        req.virtual_address = rec.vaddr;
        req.image_offset = rec.store_offset;
        req.region_id = region_id;
        lazy_.service_fault(req);

        prefetched_vaddrs_.push_back(rec.vaddr);
        ++stats_.pages_prefetched;
        if (rec.encoding == image::PageEncoding::Raw) stats_.bytes_prefetched += page_size;
        any = true;
    }
    if (any) ++stats_.regions_prefetched;
}

void PrefetchProvider::service_fault(const PageRequest& request) {
    // Reaching here means the page was not prefetched: a miss (§9.5).
    ++stats_.misses;
    lazy_.service_fault(request);
}

std::uint64_t PrefetchProvider::unused_prefetched() const {
    // A prefetched page never faults, so "used" cannot be read from the trace.
    // What the trace does prove is the misses; unused is what is left over once
    // the target has run: prefetched pages that no later fault needed. Under
    // static prefetch the honest figure is pages_prefetched minus those the
    // workload would have demanded, which only a lazy reference run can supply.
    // Reported as the raw count here; Task 8 pairs it with an oracle run.
    return stats_.pages_prefetched;
}

}  // namespace adaptive_restore::paging
