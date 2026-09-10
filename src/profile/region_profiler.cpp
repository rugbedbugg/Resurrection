#include "adaptive_restore/profile/region_profiler.hpp"

#include <algorithm>
#include <unordered_set>

#include "adaptive_restore/image/image_reader.hpp"

namespace adaptive_restore::profile {

std::vector<RegionHistory> observe_regions(const image::ImageReader& image,
                                           const std::vector<paging::FaultRecord>& trace,
                                           const std::vector<paging::RestoreAction>& actions) {
    const auto regions = image.regions();
    const auto pages = image.page_index();

    // Distinct pages faulted, per region. A page can fault only once (the
    // second touch finds it present), but count distinctly rather than relying
    // on that: a re-registered range would otherwise inflate the fraction past 1.
    std::vector<std::unordered_set<std::uint64_t>> touched(regions.size());
    for (const auto& f : trace)
        if (f.region_id < regions.size()) touched[f.region_id].insert(f.vaddr);

    std::vector<RegionHistory> out;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (i < actions.size() && actions[i] != paging::RestoreAction::Lazy)
            continue;  // unobservable; see the header

        const auto& r = regions[i];
        const std::uint32_t n = r.page_index_hi - r.page_index_lo;
        if (n == 0) continue;

        std::uint32_t zero = 0;
        for (std::uint32_t p = r.page_index_lo; p < r.page_index_hi && p < pages.size(); ++p)
            if (pages[p].encoding == image::PageEncoding::Zero) ++zero;

        RegionHistory h;
        h.region_id = r.region_id;
        h.restore_count = 1;  // this observation; the store folds it into the mean
        h.fraction_accessed_avg =
            static_cast<float>(touched[i].size()) / static_cast<float>(n);
        h.zero_page_fraction = static_cast<float>(zero) / static_cast<float>(n);
        h.last_action = paging::RestoreAction::Lazy;
        out.push_back(h);
    }
    return out;
}

}  // namespace adaptive_restore::profile
