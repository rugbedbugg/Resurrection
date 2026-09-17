#include "adaptive_restore/profile/region_profiler.hpp"

#include <algorithm>
#include <unordered_set>

#include "adaptive_restore/image/image_reader.hpp"

namespace adaptive_restore::profile {

namespace {
void set_bit(std::vector<std::uint8_t>& m, std::uint32_t i) {
    m[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
}
}  // namespace

std::vector<RegionObservation> observe_regions(
    const image::ImageReader& image, const std::vector<paging::FaultRecord>& trace,
    const std::vector<paging::RegionExecution>& execution) {
    const auto regions = image.regions();
    const auto pages = image.page_index();

    // Distinct pages faulted, per region. A page can fault only once (the
    // second touch finds it present), but count distinctly rather than relying
    // on that: a re-registered range would otherwise inflate the fraction past 1.
    std::vector<std::unordered_set<std::uint64_t>> faulted(regions.size());
    for (const auto& f : trace)
        if (f.region_id < regions.size()) faulted[f.region_id].insert(f.vaddr);

    std::vector<RegionObservation> out;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        // A region the provider never prepared, or prepared eagerly, witnessed
        // nothing: its pages were resident and reads of resident pages are
        // invisible. Skipping it is not a limitation of the profiler, it is
        // what the hardware makes available.
        if (i >= execution.size()) continue;
        const auto& ex = execution[i];
        if (ex.effective == paging::EffectiveAction::EagerWritten) continue;

        const auto& r = regions[i];
        const std::uint32_t n = r.page_index_hi - r.page_index_lo;
        if (n == 0) continue;

        RegionObservation o;
        o.region_id = r.region_id;
        o.pages = n;
        o.touched.assign((n + 7) / 8, 0);
        o.observable.assign((n + 7) / 8, 0);

        for (std::uint32_t p = 0; p < n; ++p) {
            const std::uint32_t gi = r.page_index_lo + p;
            if (gi >= pages.size()) continue;
            if (pages[gi].encoding == image::PageEncoding::Zero) ++o.zero_pages;
            if (!ex.observable(p)) continue;         // resident: UNKNOWN, not untouched
            set_bit(o.observable, p);
            if (faulted[i].count(pages[gi].vaddr)) set_bit(o.touched, p);
        }
        if (o.observable_count() > 0) out.push_back(std::move(o));
    }
    return out;
}

std::vector<RegionHistory> to_history(const std::vector<RegionObservation>& observed,
                                      std::uint32_t* skipped_partial) {
    std::vector<RegionHistory> out;
    std::uint32_t skipped = 0;
    for (const auto& o : observed) {
        if (!o.fully_observable()) { ++skipped; continue; }

        RegionHistory h;
        h.region_id = o.region_id;
        h.restore_count = 1;  // this observation; the store folds it into the mean
        h.fraction_accessed_avg =
            static_cast<float>(o.touched_count()) / static_cast<float>(o.pages);
        h.zero_page_fraction =
            static_cast<float>(o.zero_pages) / static_cast<float>(o.pages);
        h.last_action = paging::RestoreAction::Lazy;
        h.touched_bitmap = o.touched;   // every bit is real evidence here
        out.push_back(std::move(h));
    }
    if (skipped_partial) *skipped_partial = skipped;
    return out;
}

}  // namespace adaptive_restore::profile
