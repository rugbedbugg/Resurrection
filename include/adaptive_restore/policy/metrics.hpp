#pragma once
#include <cstdint>
#include <vector>

#include "adaptive_restore/paging/page_provider.hpp"
#include "adaptive_restore/policy/calibration.hpp"
#include "adaptive_restore/profile/profile_store.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::policy {

// Precision and recall of a restore's loading decision (§22, §9.5).
//
// `touched` is ground truth: under pure lazy every fault is a first touch, so a
// lazy run's fault set is exactly the set of pages the workload needs. Every
// other policy is scored against it.
struct PagingMetrics {
    std::uint64_t pages_loaded = 0;    // written, prefetched or faulted in
    std::uint64_t pages_touched = 0;   // what the workload actually needed
    std::uint64_t pages_wasted = 0;    // loaded and never needed

    // Fraction of loaded pages that were never needed. Eager's characteristic
    // failure: it loads everything, so waste rises with how little is used.
    double waste() const {
        return pages_loaded ? static_cast<double>(pages_wasted) / static_cast<double>(pages_loaded)
                            : 0.0;
    }
    // Fraction of needed pages that were resident before they were wanted.
    // Lazy's characteristic failure: it loads nothing, so coverage is 0 and
    // every needed page costs a fault.
    double coverage() const {
        if (pages_touched == 0) return 1.0;
        const std::uint64_t hits =
            pages_loaded > pages_wasted ? pages_loaded - pages_wasted : 0;
        return static_cast<double>(hits) / static_cast<double>(pages_touched);
    }
};

// The best any policy could do with perfect knowledge: for each region, the
// cheapest action given the pages that region actually needs (§12's argmin, but
// over ground truth instead of averaged history).
//
// This is what turns "beat the best fixed policy by N%" into "captured N% of
// the achievable gain". Without it a win of any size is unanchored.
struct OracleBound {
    double seconds = 0.0;
    std::uint64_t regions_eager = 0;
    std::uint64_t regions_prefetch = 0;
    std::uint64_t regions_lazy = 0;
};

// `history` must carry touched bitmaps recorded from regions left pure lazy
// (a prefetched region does not fault, so it observes nothing);
// regions without one are assumed fully touched, which is the conservative
// reading (it can only make the bound looser, never optimistic).
OracleBound oracle_bound(const image::ImageReader& image,
                         const std::vector<profile::RegionHistory>& history,
                         const Calibration& k);

// What a chosen plan actually costs, scored against ground truth rather than
// against the predictions that produced it.
//
// This is the only fair thing to compare with the oracle. A controller's own
// predicted cost is built from averaged history and can be optimistic, so
// comparing prediction to bound can produce the nonsense of a plan appearing to
// beat the bound. Scoring the chosen actions against the true touched set
// cannot: by construction the oracle picked the cheapest action per region, so
// realised >= bound always.
double plan_cost(const image::ImageReader& image,
                 const std::vector<profile::RegionHistory>& history,
                 const std::vector<paging::RestoreAction>& actions, const Calibration& k,
                 double lambda_waste);

}  // namespace adaptive_restore::policy
