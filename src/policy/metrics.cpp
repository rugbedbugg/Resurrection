#include "adaptive_restore/policy/metrics.hpp"

#include <algorithm>
#include <unordered_map>

#include "adaptive_restore/image/image_reader.hpp"

namespace adaptive_restore::policy {

OracleBound oracle_bound(const image::ImageReader& image,
                         const std::vector<profile::RegionHistory>& history,
                         const Calibration& k) {
    std::unordered_map<std::uint32_t, const profile::RegionHistory*> by_id;
    for (const auto& h : history) by_id[h.region_id] = &h;

    OracleBound out;
    const std::uint64_t page_size = image.header().page_size;

    for (const auto& r : image.regions()) {
        const std::uint32_t pages = r.page_index_hi - r.page_index_lo;
        if (pages == 0) continue;

        std::uint32_t touched = pages;  // conservative default: assume all needed
        const auto it = by_id.find(r.region_id);
        if (it != by_id.end() && !it->second->touched_bitmap.empty()) {
            touched = 0;
            for (std::uint32_t p = 0; p < pages; ++p)
                if (it->second->page_was_touched(p)) ++touched;
        }

        // With perfect knowledge there is no waste and no miss: prefetch loads
        // exactly the touched pages, lazy faults exactly them, eager loads the
        // region. The oracle simply takes the cheapest of the three.
        const double c_eager = k.cost_eager(static_cast<std::uint64_t>(pages) * page_size);
        const double c_prefetch =
            touched == 0 ? k.cost_lazy(0)
                         : k.cost_prefetch(static_cast<std::uint64_t>(touched) * page_size, 0, 0,
                                           0.0);
        const double c_lazy = k.cost_lazy(touched);

        const double best = std::min({c_eager, c_prefetch, c_lazy});
        out.seconds += best;
        // A region nobody touches is "load nothing", which is lazy. Left to the
        // tie-break it would be filed under prefetch and misreport what the
        // optimal plan actually does.
        if (touched == 0) ++out.regions_lazy;
        else if (best == c_eager) ++out.regions_eager;
        else if (best == c_prefetch) ++out.regions_prefetch;
        else ++out.regions_lazy;
    }
    return out;
}

double plan_cost(const image::ImageReader& image,
                 const std::vector<profile::RegionHistory>& history,
                 const std::vector<paging::RestoreAction>& actions, const Calibration& k,
                 double lambda_waste) {
    std::unordered_map<std::uint32_t, const profile::RegionHistory*> by_id;
    for (const auto& h : history) by_id[h.region_id] = &h;

    const std::uint64_t page_size = image.header().page_size;
    const auto regions = image.regions();
    double total = 0.0;

    for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto& r = regions[i];
        const std::uint32_t pages = r.page_index_hi - r.page_index_lo;
        if (pages == 0) continue;
        const auto action = i < actions.size() ? actions[i] : paging::RestoreAction::Eager;

        // Ground truth for this region. No record means assume fully touched,
        // which keeps the score pessimistic rather than flattering.
        std::uint32_t touched = pages, recorded = pages, hits = pages;
        const bool has_record =
            by_id.count(r.region_id) && !by_id[r.region_id]->touched_bitmap.empty();
        const auto it = by_id.find(r.region_id);
        if (has_record) {
            touched = recorded = hits = 0;
            for (std::uint32_t p = 0; p < pages; ++p)
                if (it->second->page_was_touched(p)) {
                    ++touched;
                    ++recorded;
                    ++hits;
                }
        }

        switch (action) {
        case paging::RestoreAction::Eager:
            total += k.cost_eager(static_cast<std::uint64_t>(pages) * page_size);
            break;
        case paging::RestoreAction::StaticPrefetch: {
            // With no recorded set the provider prefetches nothing and behaves
            // as pure lazy (prefetch_provider.cpp). Pricing it as a full
            // prefetch overstated the plan on exactly the regions the
            // controller knows least about, and this number is printed as the
            // share of achievable gain.
            if (!has_record) {
                total += k.cost_lazy(touched);
                break;
            }
            const std::uint32_t misses = touched > hits ? touched - hits : 0;
            const std::uint32_t wasted = recorded > hits ? recorded - hits : 0;
            total += k.cost_prefetch(static_cast<std::uint64_t>(recorded) * page_size, misses,
                                     static_cast<std::uint64_t>(wasted) * page_size,
                                     lambda_waste);
            break;
        }
        case paging::RestoreAction::Lazy:
            total += k.cost_lazy(touched);
            break;
        }
    }
    return total;
}

}  // namespace adaptive_restore::policy
