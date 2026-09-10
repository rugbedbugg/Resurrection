#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "adaptive_restore/policy/plan.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::tui {

// §19: the visualizer consumes logs and images. It never participates in a
// restore, so nothing here may touch a process, a uffd, or the clock.
struct Style {
    bool color = true;
    std::size_t width = 80;
};

// One demand fault, as written by `restore --record-trace`.
struct FaultRow {
    std::uint64_t vaddr = 0;
    std::uint32_t region = 0;
    std::uint64_t offset_ns = 0;
    std::uint64_t service_ns = 0;
    std::uint32_t bytes = 0;
    std::uint32_t encoding = 0;
};

// One measured restore, as written by `arctl benchmark` into raw.tsv.
struct BenchRow {
    std::string workload, policy, cache;
    double restore_ms = 0.0;
    std::uint64_t faults = 0;
    bool output_ok = false;
};

// §19.2 view 1: the address space, one cell per region, coloured by the action
// the plan chose for it.
void render_region_map(std::ostream& os, const image::ImageReader& image,
                       const policy::RestorePlan& plan, const Style& style);

// §19.2 view 2: when demand faults arrived, against the time the restore spent
// writing pages up front.
void render_fault_timeline(std::ostream& os, const std::vector<FaultRow>& faults,
                           const Style& style);

// §19.2 view 3: the pitch. Median resume latency per policy, side by side, per
// workload, with the per-workload winner marked.
void render_policy_comparison(std::ostream& os, const std::vector<BenchRow>& rows,
                              const Style& style);

}  // namespace adaptive_restore::tui
