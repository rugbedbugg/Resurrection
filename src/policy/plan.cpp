#include "adaptive_restore/policy/plan.hpp"

#include <algorithm>
#include <memory>
#include <unordered_map>

#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/paging/lazy_ranges.hpp"
#include "adaptive_restore/policy/policy_engine.hpp"

namespace adaptive_restore::policy {

RegionFeatures features_for(const image::RegionRecord& r,
                            const std::vector<image::MappingRecord>& mappings,
                            const std::vector<image::PageIndexRecord>& pages,
                            std::uint32_t page_size, const profile::RegionHistory* h) {
    RegionFeatures f;
    f.region_id = r.region_id;
    f.pages = r.page_index_hi - r.page_index_lo;
    f.page_size = page_size;

    for (const auto& m : mappings)
        if (r.start_vaddr >= m.start_vaddr && r.end_vaddr <= m.end_vaddr) {
            f.file_backed = (m.flags & image::MappingFlag::FileBacked) != 0;
            f.executable = (m.flags & image::MappingFlag::Exec) != 0;
            f.stack_or_heap = m.path == "[stack]" || m.path == "[heap]";
            break;
        }

    std::uint32_t zero = 0;
    for (std::uint32_t p = r.page_index_lo; p < r.page_index_hi && p < pages.size(); ++p)
        if (pages[p].encoding == image::PageEncoding::Zero) ++zero;
    f.zero_page_fraction = f.pages ? static_cast<float>(zero) / static_cast<float>(f.pages) : 0.0f;

    if (h) {
        f.fraction_accessed_avg = h->fraction_accessed_avg;
        f.restore_count = h->restore_count;
        std::uint32_t ws = 0;
        for (std::uint32_t p = 0; p < f.pages; ++p)
            if (h->page_was_touched(p)) ++ws;
        f.recorded_working_set = ws;
    }
    return f;
}

RestorePlan plan_restore(const image::ImageReader& image,
                         const std::vector<profile::RegionHistory>& history,
                         RestorePolicy policy, bool cold_cache) {
    const auto regions = image.regions();
    const auto mappings = image.mappings();
    const auto pages = image.page_index();
    const auto ranges = paging::lazy_ranges(image);
    const std::uint32_t page_size = image.header().page_size;

    // Only private anonymous regions can be served by userfaultfd MISSING mode,
    // so everything else is eager no matter what the policy asks for.
    const auto uffd_eligible = [&ranges](const image::RegionRecord& r) {
        for (const auto& lr : ranges)
            if (r.start_vaddr >= lr.start && r.end_vaddr <= lr.end) return true;
        return false;
    };

    std::unordered_map<std::uint32_t, const profile::RegionHistory*> by_id;
    for (const auto& h : history) by_id[h.region_id] = &h;

    RestorePlan out;
    out.actions.assign(regions.size(), paging::RestoreAction::Eager);

    const auto cold_cal = Calibration::defaults_cold();
    const auto warm_cal = Calibration::defaults_warm();
    std::unique_ptr<CostModelPolicyEngine> engine;
    if (policy == RestorePolicy::Cost)
        engine = std::make_unique<CostModelPolicyEngine>(cold_cal, warm_cal);

    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (!uffd_eligible(regions[i])) continue;  // stays eager
        switch (policy) {
        case RestorePolicy::Eager:
            break;
        case RestorePolicy::Lazy:
            out.actions[i] = paging::RestoreAction::Lazy;
            break;
        case RestorePolicy::Prefetch: {
            // Requesting a prefetch the provider cannot perform produces a
            // region that silently behaves as lazy while still being labelled
            // StaticPrefetch. Decide it here, where the outcome is knowable,
            // so requested and effective action agree.
            const auto it = by_id.find(regions[i].region_id);
            const bool usable = it != by_id.end() && !it->second->touched_bitmap.empty();
            out.actions[i] = usable ? paging::RestoreAction::StaticPrefetch
                                    : paging::RestoreAction::Lazy;
            break;
        }
        case RestorePolicy::Cost: {
            const auto it = by_id.find(regions[i].region_id);
            const auto f = features_for(regions[i], mappings, pages, page_size,
                                        it == by_id.end() ? nullptr : it->second);
            const auto d = engine->choose(f, SystemState{cold_cache});
            out.actions[i] = d.action;
            out.predicted_transfer_s += d.predicted_cost;
            out.decisions.push_back(d);
            break;
        }
        }
    }

    // Deferring anything costs a fixed setup (uffd, page server, THP disabling)
    // that §12 does not model. If the whole plan does not save more than that,
    // the honest answer is to defer nothing: paying 8 ms of machinery to save
    // 1 ms of transfer is a loss dressed up as a decision.
    if (policy == RestorePolicy::Cost && engine) {
        const auto& kcal = cold_cache ? cold_cal : warm_cal;
        const auto region_bytes = [page_size](const image::RegionRecord& r) {
            return static_cast<std::uint64_t>(r.page_index_hi - r.page_index_lo) * page_size;
        };
        double plan = 0.0, all_eager = 0.0;
        for (const auto& r : regions) all_eager += kcal.cost_eager(region_bytes(r));
        for (const auto& d : out.decisions) plan += d.predicted_cost;
        // Regions that were never offered to the engine are eager either way.
        for (const auto& r : regions)
            if (!uffd_eligible(r)) plan += kcal.cost_eager(region_bytes(r));
        if (all_eager - plan < kcal.deferral_setup_s) {
            std::fill(out.actions.begin(), out.actions.end(), paging::RestoreAction::Eager);
            out.decisions.clear();
            // The prediction must describe the plan that actually runs, not the
            // one just discarded.
            out.predicted_transfer_s = all_eager;
            out.deferral_declined = true;
        }
    }
    return out;
}

}  // namespace adaptive_restore::policy
