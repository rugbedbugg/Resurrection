#pragma once
#include <cstdint>

#include "adaptive_restore/paging/page_provider.hpp"

namespace adaptive_restore::policy {

// The Tier 1 feature set. Deliberately small: everything here is either
// read straight out of the image or averaged from the profile, so a decision can
// be explained by pointing at the numbers that produced it.
struct RegionFeatures {
    std::uint32_t region_id = 0;
    std::uint32_t pages = 0;                  // region size in pages
    std::uint32_t page_size = 4096;

    float fraction_accessed_avg = 1.0f;       // averaged over prior restores
    float zero_page_fraction = 0.0f;
    std::uint32_t recorded_working_set = 0;   // pages in the last observed set

    bool file_backed = false;
    bool executable = false;
    bool stack_or_heap = false;

    // 0 means no history for this identity. The controller must not read
    // fraction_accessed_avg as evidence when this is 0; the default of 1.0 is
    // an assumption (treat as dense), not a measurement.
    std::uint64_t restore_count = 0;

    std::uint64_t bytes() const {
        return static_cast<std::uint64_t>(pages) * page_size;
    }
};

// What actually happened, fed back after a restore.
struct RegionOutcome {
    std::uint32_t pages_touched = 0;
    std::uint32_t pages_loaded = 0;
    double observed_seconds = 0.0;
};

struct RegionDecision {
    std::uint32_t region_id = 0;
    paging::RestoreAction action = paging::RestoreAction::Eager;
    float confidence = 0.0f;
    float predicted_cost = 0.0f;   // seconds, the winning arm's cost

    // Why, in numbers. This controller is meant to be explainable, so the
    // losing arms travel with the winner.
    float cost_eager = 0.0f;
    float cost_prefetch = 0.0f;
    float cost_lazy = 0.0f;
};

// Conditions the decision is made under. Calibration showed cache state moves
// the eager/lazy crossover by 6x, so it is not optional context.
struct SystemState {
    bool cold_cache = true;
};

}  // namespace adaptive_restore::policy
