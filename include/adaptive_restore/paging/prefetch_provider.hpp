#pragma once
#include <cstdint>
#include <vector>

#include "adaptive_restore/image/format.hpp"
#include <string>
#include <unordered_map>
#include <vector>

#include "adaptive_restore/paging/lazy_provider.hpp"
#include "adaptive_restore/paging/page_provider.hpp"
#include "adaptive_restore/profile/profile_store.hpp"
#include "adaptive_restore/restore/target.hpp"
#include "adaptive_restore/restore/uffd.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::paging {

// §9.5 asks for these explicitly: what the prediction got right, what it missed,
// and what it loaded for nothing. The last two are precision and recall of the
// recorded working set, and are what Phase 5 reports as waste and coverage.
struct PrefetchStats {
    std::uint64_t regions_prefetched = 0;
    std::uint64_t pages_prefetched = 0;    // written up front from history
    std::uint64_t bytes_prefetched = 0;
    std::uint64_t hits = 0;                // prefetched AND later needed
    std::uint64_t misses = 0;              // needed but not prefetched, so faulted
    std::uint64_t unused = 0;              // prefetched but never demanded
    std::uint64_t regions_without_history = 0;  // degraded to pure lazy
    std::uint64_t already_present = 0;           // prefetch was a no-op; see EEXIST above
    std::uint64_t pages_held_out = 0;            // EXPERIMENTAL: see HoldoutConfig
};

// EXPERIMENTAL (Phase 6A spike, off by default).
//
// Withhold k pages of each region's predicted set from prefetching so they
// stay absent and therefore fault if the workload still wants them. The fault
// rate over the held-out sample estimates the prediction's precision, which is
// otherwise unobservable: a prefetched page is resident and reveals nothing.
//
// This is instrumentation, NOT policy. The planner does not know it exists and
// nothing enables it automatically; `arctl` reads it from the environment so a
// spike script can sweep k without a production flag existing.
struct HoldoutConfig {
    std::uint32_t k = 0;      // pages withheld per region; 0 disables entirely
    std::uint64_t seed = 0;   // recorded with every result for reproducibility

    static HoldoutConfig from_env();   // AR_EXPERIMENTAL_HOLDOUT_{K,SEED}
    bool enabled() const { return k > 0; }
};

// One withheld page and what became of it.
struct HoldoutRecord {
    std::uint32_t region_id = 0;
    std::uint32_t page_index = 0;   // within the region
    std::uint64_t vaddr = 0;
};

// Loads the historically-touched subset of a region up front and leaves the
// rest to fault (§9.5).
//
// The whole region is registered regardless of what was prefetched, so a wrong
// prediction costs latency and never correctness: §11.4's safety fallback. With
// no history a region degrades to pure lazy rather than failing, so a cold
// profile is a slow first run, not a broken one.
class PrefetchProvider : public PageProvider {
public:
    PrefetchProvider(const image::ImageReader& image, restore::RestoreTarget& target,
                     restore::Userfault& uffd,
                     const std::vector<profile::RegionHistory>& history);

    void prepare_region(std::uint32_t region_id, RestoreAction action) override;
    void service_fault(const PageRequest& request) override;

    const PageLocator& locator() const { return lazy_.locator(); }
    const PrefetchStats& stats() const { return stats_; }
    const LazyStats& lazy_stats() const { return lazy_.stats(); }
    const std::vector<FaultRecord>& trace() const { return lazy_.trace(); }

    // What each region's execution actually was, indexed by region id. The
    // profiler reads this instead of the requested action.
    const std::vector<RegionExecution>& executions() const { return exec_; }

    // EXPERIMENTAL: pages deliberately withheld this run.
    const std::vector<HoldoutRecord>& holdouts() const { return holdouts_; }
    void set_holdout(const HoldoutConfig& h) { holdout_ = h; }
    void record_trace(bool on) { lazy_.record_trace(on); }
    void write_trace_to(const std::string& path) const { lazy_.write_trace(path); }

    // Pages loaded up front that no fault ever asked for. Only meaningful once
    // the target has exited.
    std::uint64_t unused_prefetched() const;

private:
    // Prefetching resolves contiguous runs in one ioctl, so the uffd is needed
    // directly rather than through LazyProvider's per-page path.
    std::uint64_t copy_run(std::uint64_t vaddr, const std::byte* src, std::uint64_t len);
    std::uint64_t zero_run(std::uint64_t vaddr, std::uint64_t len);

    const image::ImageReader& image_;
    restore::RestoreTarget& target_;
    restore::Userfault& uffd_;

    // Cached once. ImageReader parses each section on demand, so reading them
    // per region turns a linear restore quadratic: 283 regions x 17086 page
    // records measured 212 ms of pure parsing. This is the second time this
    // trap has been hit (EagerProvider had it too), hence the loud comment.
    std::vector<image::RegionRecord> regions_;
    std::vector<image::PageIndexRecord> pages_;
    std::uint64_t page_size_ = 4096;

    LazyProvider lazy_;  // owns registration and fault resolution
    std::unordered_map<std::uint32_t, profile::RegionHistory> history_;
    std::vector<std::uint64_t> prefetched_vaddrs_;
    std::vector<RegionExecution> exec_;
    HoldoutConfig holdout_;
    std::vector<HoldoutRecord> holdouts_;
    PrefetchStats stats_;
};

}  // namespace adaptive_restore::paging
