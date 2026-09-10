#include "adaptive_restore/policy/policy_engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace adaptive_restore::policy {

CostModelPolicyEngine::CostModelPolicyEngine(Calibration cold, Calibration warm,
                                             double lambda_waste)
    : cold_(cold), warm_(warm), lambda_waste_(lambda_waste) {
    if (lambda_waste_ < 0.0) {
        const double bw = cold_.uffd_copy_bandwidth_bps;
        lambda_waste_ = bw > 0 ? 1.0 / bw : 0.0;
    }
}

RegionDecision CostModelPolicyEngine::choose(const RegionFeatures& f, const SystemState& s) {
    const Calibration& k = s.cold_cache ? cold_ : warm_;

    // With no history, fraction_accessed_avg is an assumption rather than a
    // measurement. Assume dense: eager is always correct and merely costs
    // bandwidth, whereas guessing sparse and being wrong costs a fault storm.
    const bool has_history = f.restore_count > 0;
    const double fraction = has_history ? std::clamp(f.fraction_accessed_avg, 0.0f, 1.0f) : 1.0f;
    const double expected_touched = fraction * f.pages;

    RegionDecision d;
    d.region_id = f.region_id;
    d.cost_eager = static_cast<float>(k.cost_eager(f.bytes()));

    // Zero pages never cost a read; they are supplied by UFFDIO_ZEROPAGE.
    const double faulting_pages = expected_touched;
    d.cost_lazy = static_cast<float>(
        k.cost_lazy(static_cast<std::uint64_t>(std::llround(faulting_pages))));

    // Prefetch is only meaningful with a recorded working set to load.
    if (has_history && f.recorded_working_set > 0) {
        const double prefetch_pages = f.recorded_working_set;
        // The recorded set is the prediction; assume it predicts as well as its
        // own size allows. Hits cannot exceed either the set or what is touched.
        const double hits = std::min(prefetch_pages, expected_touched);
        const double misses = std::max(0.0, expected_touched - hits);
        const double wasted_pages = std::max(0.0, prefetch_pages - hits);

        d.cost_prefetch = static_cast<float>(k.cost_prefetch(
            static_cast<std::uint64_t>(prefetch_pages) * f.page_size,
            static_cast<std::uint64_t>(std::llround(misses)),
            static_cast<std::uint64_t>(wasted_pages) * f.page_size, lambda_waste_));
    } else {
        d.cost_prefetch = std::numeric_limits<float>::infinity();
    }

    // argmin (§12).
    d.action = paging::RestoreAction::Eager;
    d.predicted_cost = d.cost_eager;
    if (d.cost_prefetch < d.predicted_cost) {
        d.action = paging::RestoreAction::StaticPrefetch;
        d.predicted_cost = d.cost_prefetch;
    }
    if (d.cost_lazy < d.predicted_cost) {
        d.action = paging::RestoreAction::Lazy;
        d.predicted_cost = d.cost_lazy;
    }

    // Tier 1 confidence is a fixed heuristic (§6.2): how much history backs the
    // fraction this decision rests on. Tier 2 learns it.
    d.confidence = !has_history ? 0.0f
                                : static_cast<float>(std::min(1.0, static_cast<double>(f.restore_count) / 5.0));
    return d;
}

void CostModelPolicyEngine::update(const RegionFeatures&, paging::RestoreAction,
                                   const RegionOutcome&) {
    // The analytical controller does not learn at decision time; the region
    // profiler records outcomes and the next restore reads them. Tier 2's
    // bandit and neural engines are what this hook exists for.
}

}  // namespace adaptive_restore::policy
