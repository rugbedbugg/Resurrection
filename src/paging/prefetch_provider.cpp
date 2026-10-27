#include "adaptive_restore/paging/prefetch_provider.hpp"

#include <linux/userfaultfd.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/paging/transfer.hpp"

namespace adaptive_restore::paging {

HoldoutConfig HoldoutConfig::from_env() {
    HoldoutConfig h;
    if (const char* k = std::getenv("AR_EXPERIMENTAL_HOLDOUT_K"))
        h.k = static_cast<std::uint32_t>(std::strtoul(k, nullptr, 10));
    if (const char* s = std::getenv("AR_EXPERIMENTAL_HOLDOUT_SEED"))
        h.seed = std::strtoull(s, nullptr, 10);
    return h;
}

namespace {

// Deterministic per-(seed, region, page) hash. Uniform selection without
// materialising or shuffling the predicted set: a page is held out when its
// hash falls in the lowest k/|predicted| of the range. Reproducible from the
// seed alone, which is what makes a variance sweep meaningful.
std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}
}  // namespace

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

    if (exec_.size() <= region_id) exec_.resize(region_id + 1);
    // Until a page is actually written, the honest description is "nothing
    // populated". Every early return below therefore leaves this correct.
    exec_[region_id].effective = EffectiveAction::FullyDeferred;

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
        // exec_ already says FullyDeferred, which is what makes these regions
        // observable to the profiler instead of being discarded as "prefetch".
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
    // EXPERIMENTAL holdout: decide up front which predicted pages to withhold,
    // so the run-coalescing below simply treats them as not-predicted.
    std::vector<std::uint8_t> withheld;
    if (holdout_.enabled()) {
        std::uint32_t predicted = 0;
        for (std::uint32_t i = 0; i < n; ++i)
            if (it->second.page_was_touched(i)) ++predicted;
        if (predicted > 0) {
            const std::uint32_t k = std::min(holdout_.k, predicted);
            // Rank the predicted pages by hash and withhold the lowest k. This
            // is a uniform sample of the predicted set, not of the region.
            std::vector<std::pair<std::uint64_t, std::uint32_t>> ranked;
            ranked.reserve(predicted);
            for (std::uint32_t i = 0; i < n; ++i)
                if (it->second.page_was_touched(i))
                    ranked.emplace_back(
                        splitmix64(holdout_.seed ^ (static_cast<std::uint64_t>(region_id) << 32) ^ i),
                        i);
            std::partial_sort(ranked.begin(), ranked.begin() + k, ranked.end());
            withheld.assign((n + 7) / 8, 0);
            for (std::uint32_t j = 0; j < k; ++j) {
                const std::uint32_t pi = ranked[j].second;
                withheld[pi / 8] |= static_cast<std::uint8_t>(1u << (pi % 8));
                const std::uint32_t gi = r.page_index_lo + pi;
                holdouts_.push_back({region_id, pi,
                                     gi < pages.size() ? pages[gi].vaddr : 0});
            }
            stats_.pages_held_out += k;
        }
    }
    const auto predicted_now = [&](std::uint32_t i) {
        if (!it->second.page_was_touched(i)) return false;
        if (!withheld.empty() && (withheld[i / 8] & (1u << (i % 8)))) return false;
        return true;
    };

    auto& ex = exec_[region_id];
    ex.populated.assign((n + 7) / 8, 0);

    bool any = false;
    std::uint32_t p = 0;
    while (p < n) {
        if (!predicted_now(p)) { ++p; continue; }
        const std::uint32_t gi = r.page_index_lo + p;
        if (gi >= pages.size()) { ++p; continue; }

        const auto& first = pages[gi];
        // Zero pages resolve with a different ioctl, so runs never mix them.
        const bool zero_run_kind = first.encoding == image::PageEncoding::Zero;

        std::uint32_t q = p + 1;
        while (q < n && predicted_now(q)) {
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

        for (std::uint32_t k = p; k < q; ++k) {
            prefetched_vaddrs_.push_back(pages[r.page_index_lo + k].vaddr);
            ex.populated[k / 8] |= static_cast<std::uint8_t>(1u << (k % 8));
        }
        stats_.pages_prefetched += run_pages;
        any = true;
        p = q;
    }
    // Only a region that actually wrote something is PrefetchedPartial; one
    // whose whole predicted set was withheld or empty stays FullyDeferred.
    if (any) {
        ++stats_.regions_prefetched;
        ex.effective = EffectiveAction::PrefetchedPartial;
    } else {
        ex.populated.clear();
    }
}

void PrefetchProvider::service_fault(const PageRequest& request) {
    // Reaching here means the page was not prefetched: a miss.
    ++stats_.misses;
    lazy_.service_fault(request);
}

std::uint64_t PrefetchProvider::copy_run(std::uint64_t vaddr, const std::byte* src,
                                        std::uint64_t len) {
    ::uffdio_copy c{};
    c.dst = vaddr;
    c.src = reinterpret_cast<std::uint64_t>(src);
    c.len = len;
    if (::ioctl(uffd_.fd(), UFFDIO_COPY, &c) != 0) {
        // EEXIST is NOT benign. It means a page this code believed missing is
        // already resident, so its model of the address space is wrong and the
        // invariant that every saved page is written or covered fails.
        // Skipping the copy and carrying on was tried, and produced a target
        // that ran on stale memory and died with heap corruption -- silent
        // wrongness, the worst outcome there is.
        // Fail explicitly, never execute partially-restored state.
        ++stats_.already_present;
        throw CheckpointError(
            "restore: page at " + hex(vaddr) +
            " was already resident when prefetching " + std::to_string(len) +
            " bytes (" + std::strerror(errno) +
            "); the restore's model of the address space is inconsistent and "
            "continuing would run the target on stale memory");
    }
    return static_cast<std::uint64_t>(c.copy);
}

std::uint64_t PrefetchProvider::zero_run(std::uint64_t vaddr, std::uint64_t len) {
    ::uffdio_zeropage z{};
    z.range.start = vaddr;
    z.range.len = len;
    if (::ioctl(uffd_.fd(), UFFDIO_ZEROPAGE, &z) != 0) {
        ++stats_.already_present;
        throw CheckpointError(
            "restore: page at " + hex(vaddr) + " was already resident when zeroing " +
            std::to_string(len) + " bytes (" + std::strerror(errno) +
            "); the restore's model of the address space is inconsistent and "
            "continuing would run the target on stale memory");
    }
    return static_cast<std::uint64_t>(z.zeropage);
}

std::uint64_t PrefetchProvider::unused_prefetched() const {
    // A prefetched page never faults, so "used" cannot be read from the trace.
    // What the trace does prove is the misses; unused is what is left over once
    // the target has run: prefetched pages that no later fault needed. Under
    // static prefetch the honest figure is pages_prefetched minus those the
    // workload would have demanded, which only a lazy reference run can supply.
    // Reported as the raw count here; oracle_bound() supplies the reference.
    return stats_.pages_prefetched;
}

}  // namespace adaptive_restore::paging
