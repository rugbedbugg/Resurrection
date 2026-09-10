#pragma once
#include <cstdint>
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
    void record_trace(bool on) { lazy_.record_trace(on); }

    // Pages loaded up front that no fault ever asked for. Only meaningful once
    // the target has exited.
    std::uint64_t unused_prefetched() const;

private:
    const image::ImageReader& image_;
    restore::RestoreTarget& target_;
    LazyProvider lazy_;  // owns registration and fault resolution
    std::unordered_map<std::uint32_t, profile::RegionHistory> history_;
    std::vector<std::uint64_t> prefetched_vaddrs_;
    PrefetchStats stats_;
};

}  // namespace adaptive_restore::paging
