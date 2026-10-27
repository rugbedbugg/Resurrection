#pragma once
#include <cstdint>
#include <vector>

#include "adaptive_restore/paging/page_provider.hpp"
#include "adaptive_restore/policy/calibration.hpp"
#include "adaptive_restore/policy/region.hpp"
#include "adaptive_restore/profile/profile_store.hpp"

namespace adaptive_restore::image {
class ImageReader;
struct RegionRecord;
struct MappingRecord;
struct PageIndexRecord;
}

namespace adaptive_restore::policy {

enum class RestorePolicy {
    Eager,     // every page written before the target resumes
    Lazy,      // anonymous regions registered and served on demand
    Prefetch,  // load the recorded working set, fault for the rest
    Cost,      // choose per region from the cost model
};

// Build the Tier 1 feature set for one region, from what the image says
// and what the profile remembers. `h` may be null: no history for this region.
RegionFeatures features_for(const image::RegionRecord& r,
                            const std::vector<image::MappingRecord>& mappings,
                            const std::vector<image::PageIndexRecord>& pages,
                            std::uint32_t page_size, const profile::RegionHistory* h);

// One action per region, in image order, plus the reasoning behind it.
struct RestorePlan {
    std::vector<paging::RestoreAction> actions;
    std::vector<RegionDecision> decisions;  // populated under Cost only
    double predicted_transfer_s = 0.0;      // sum of the chosen arms' costs
    bool deferral_declined = false;         // the plan did not repay its setup cost
};

// Decide what to do with every region, without touching a process.
//
// Deliberately separate from restore_image(): `arctl visualize` must be able to
// show the plan a restore *would* choose, and a visualizer that re-derived the
// decisions itself would drift from the restorer the first time either changed.
// The restorer calls this too, so what is drawn is what would run.
RestorePlan plan_restore(const image::ImageReader& image,
                         const std::vector<profile::RegionHistory>& history,
                         RestorePolicy policy, bool cold_cache);

}  // namespace adaptive_restore::policy
