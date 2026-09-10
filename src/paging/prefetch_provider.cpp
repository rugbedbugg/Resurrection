#include "adaptive_restore/paging/prefetch_provider.hpp"

#include <linux/userfaultfd.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <unordered_set>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/paging/transfer.hpp"

namespace adaptive_restore::paging {

PrefetchProvider::PrefetchProvider(const image::ImageReader& image,
                                   restore::RestoreTarget& target, restore::Userfault& uffd,
                                   const std::vector<profile::RegionHistory>& history)
    : image_(image),
      target_(target),
      uffd_(uffd),
      regions_(image.regions()),
      pages_(image.page_index()),
      page_size_(image.header().page_size),
      lazy_(image, uffd) {
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

    const auto& r = regions_[region_id];
    const auto& pages = pages_;
    const std::uint64_t page_size = page_size_;
    const std::uint32_t n = r.page_index_hi - r.page_index_lo;

    // Coalesce runs contiguous in both the target and the image store, exactly
    // as the eager path does. UFFDIO_COPY takes a length, so a 64-page run is
    // one ioctl rather than 64. Resolving page by page costs a full fault-time
    // round trip each (~26us measured), which made prefetch slower than eager
    // even when it loaded a fraction of the data.
    bool any = false;
    std::uint32_t p = 0;
    while (p < n) {
        if (!it->second.page_was_touched(p)) { ++p; continue; }
        const std::uint32_t gi = r.page_index_lo + p;
        if (gi >= pages.size()) { ++p; continue; }

        const auto& first = pages[gi];
        // Zero pages resolve with a different ioctl, so runs never mix them.
        const bool zero_run_kind = first.encoding == image::PageEncoding::Zero;

        std::uint32_t q = p + 1;
        while (q < n && it->second.page_was_touched(q)) {
            const std::uint32_t gq = r.page_index_lo + q;
            if (gq >= pages.size()) break;
            const auto& cur = pages[gq];
            const auto& prev = pages[gq - 1];
            if ((cur.encoding == image::PageEncoding::Zero) != zero_run_kind) break;
            if (cur.vaddr != prev.vaddr + page_size) break;
            if (!zero_run_kind && cur.store_offset != prev.store_offset + prev.store_len) break;
            ++q;
        }

        const std::uint64_t run_pages = q - p;
        const std::uint64_t len = run_pages * page_size;
        if (zero_run_kind) {
            zero_run(first.vaddr, len);
        } else if (first.encoding == image::PageEncoding::Raw) {
            copy_run(first.vaddr, image_.page_bytes(first).data(), len);
            stats_.bytes_prefetched += len;
        } else {
            // FromFile inside a lazy range should not happen: only anonymous
            // regions are deferred, and a referenced page lives in a file.
            throw CheckpointError("prefetch: a file-referenced page was deferred");
        }

        for (std::uint32_t k = p; k < q; ++k)
            prefetched_vaddrs_.push_back(pages[r.page_index_lo + k].vaddr);
        stats_.pages_prefetched += run_pages;
        any = true;
        p = q;
    }
    if (any) ++stats_.regions_prefetched;
}

void PrefetchProvider::service_fault(const PageRequest& request) {
    // Reaching here means the page was not prefetched: a miss (§9.5).
    ++stats_.misses;
    lazy_.service_fault(request);
}

std::uint64_t PrefetchProvider::copy_run(std::uint64_t vaddr, const std::byte* src,
                                        std::uint64_t len) {
    ::uffdio_copy c{};
    c.dst = vaddr;
    c.src = reinterpret_cast<std::uint64_t>(src);
    c.len = len;
    if (::ioctl(uffd_.fd(), UFFDIO_COPY, &c) != 0)
        throw CheckpointError(std::string("prefetch: UFFDIO_COPY: ") + std::strerror(errno));
    return static_cast<std::uint64_t>(c.copy);
}

std::uint64_t PrefetchProvider::zero_run(std::uint64_t vaddr, std::uint64_t len) {
    ::uffdio_zeropage z{};
    z.range.start = vaddr;
    z.range.len = len;
    if (::ioctl(uffd_.fd(), UFFDIO_ZEROPAGE, &z) != 0)
        throw CheckpointError(std::string("prefetch: UFFDIO_ZEROPAGE: ") + std::strerror(errno));
    return static_cast<std::uint64_t>(z.zeropage);
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
