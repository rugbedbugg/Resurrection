#include "adaptive_restore/restore/restore.hpp"

#include <time.h>

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/common/version.hpp"
#include "adaptive_restore/paging/eager_provider.hpp"
#include "adaptive_restore/policy/policy_engine.hpp"
#ifdef AR_HAVE_SQLITE
#include "adaptive_restore/profile/profile_store.hpp"
#endif

namespace adaptive_restore::restore {

namespace {

std::uint64_t monotonic_ns() {
    ::timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

}  // namespace

paging::ServeResult LazySession::serve(pid_t target_pid) {
    return paging::serve_faults(*uffd, *provider, provider->locator(), target_pid);
}

namespace {

// Build the Tier 1 feature set (§10.1) for one region, from what the image says
// and what the profile remembers.
policy::RegionFeatures features_for(const image::RegionRecord& r,
                                    const std::vector<image::MappingRecord>& mappings,
                                    const std::vector<image::PageIndexRecord>& pages,
                                    std::uint32_t page_size,
                                    const profile::RegionHistory* h) {
    policy::RegionFeatures f;
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

}  // namespace

RestoreSummary restore_image(const image::ImageReader& image, const RestoreOptions& opts) {
    const std::uint64_t started = monotonic_ns();

    RestoreSummary out;
    auto target = make_target_for(image.mappings(), opts.reserve_addr, opts.reserve_bytes);
    out.pid = target->pid();

    out.layout = rebuild_address_space(*target, image);

    const auto regions = image.regions();
    const auto mappings = image.mappings();
    const auto pages = image.page_index();
    const auto ranges = lazy_ranges(image);

    // Only private anonymous regions can be served by userfaultfd MISSING mode,
    // so everything else is eager no matter what the policy asks for.
    const auto uffd_eligible = [&ranges](const image::RegionRecord& r) {
        for (const auto& lr : ranges)
            if (r.start_vaddr >= lr.start && r.end_vaddr <= lr.end) return true;
        return false;
    };

    const auto& history = opts.history;  // loaded by the caller, before the clock
    std::unordered_map<std::uint32_t, const profile::RegionHistory*> by_id;
    for (const auto& h : history) by_id[h.region_id] = &h;

    // --- decide an action per region ----------------------------------------
    std::vector<paging::RestoreAction> actions(regions.size(), paging::RestoreAction::Eager);
    const auto cold_cal = policy::Calibration::defaults_cold();
    const auto warm_cal = policy::Calibration::defaults_warm();
    std::unique_ptr<policy::CostModelPolicyEngine> engine;
    if (opts.policy == RestorePolicy::Cost)
        engine = std::make_unique<policy::CostModelPolicyEngine>(cold_cal, warm_cal);

    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (!uffd_eligible(regions[i])) continue;  // stays eager
        switch (opts.policy) {
        case RestorePolicy::Eager:
            break;
        case RestorePolicy::Lazy:
            actions[i] = paging::RestoreAction::Lazy;
            break;
        case RestorePolicy::Prefetch:
            actions[i] = paging::RestoreAction::StaticPrefetch;
            break;
        case RestorePolicy::Cost: {
            const auto it = by_id.find(regions[i].region_id);
            const auto f = features_for(regions[i], mappings, pages, image.header().page_size,
                                        it == by_id.end() ? nullptr : it->second);
            const auto d = engine->choose(f, policy::SystemState{opts.cold_cache});
            actions[i] = d.action;
            out.predicted_transfer_s += d.predicted_cost;
            out.decisions.push_back(d);
            break;
        }
        }
    }

    // Any 2 MiB span that will hold a deferred page must lose THP first, or the
    // kernel can make neighbouring pages resident behind the restore's back.
    //
    // Scoped to the SPAN, not the whole mapping. THP works in 2 MiB units, so
    // that is the smallest granularity that actually prevents the hazard, and
    // disabling it mapping-wide penalises the eager regions sharing that
    // mapping. `cost` is the only policy that mixes both in one mapping, so a
    // mapping-wide sweep hurt precisely the policy under test: it went from
    // beating prefetch by 16% to losing to it by 15%.
    {
        constexpr std::uint64_t kTHP = 2u * 1024u * 1024u;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
        for (std::size_t i = 0; i < regions.size(); ++i) {
            if (actions[i] == paging::RestoreAction::Eager) continue;
            const auto& r = regions[i];
            std::uint64_t lo = r.start_vaddr & ~(kTHP - 1);
            std::uint64_t hi = (r.end_vaddr + kTHP - 1) & ~(kTHP - 1);
            for (const auto& m : mappings)
                if (r.start_vaddr >= m.start_vaddr && r.end_vaddr <= m.end_vaddr) {
                    lo = std::max(lo, m.start_vaddr);
                    hi = std::min(hi, m.end_vaddr);
                    break;
                }
            if (hi > lo) spans.emplace_back(lo, hi);
        }
        std::sort(spans.begin(), spans.end());
        std::uint64_t cur_lo = 0, cur_hi = 0;
        for (const auto& [lo, hi] : spans) {
            if (cur_hi == 0) { cur_lo = lo; cur_hi = hi; continue; }
            if (lo <= cur_hi) { cur_hi = std::max(cur_hi, hi); continue; }
            disable_huge_pages(*target, cur_lo, cur_hi);
            cur_lo = lo; cur_hi = hi;
        }
        if (cur_hi > cur_lo) disable_huge_pages(*target, cur_lo, cur_hi);
    }

    // Deferring anything costs a fixed setup (uffd, page server, THP disabling)
    // that §12 does not model. If the whole plan does not save more than that,
    // the honest answer is to defer nothing: paying 8 ms of machinery to save
    // 1 ms of transfer is a loss dressed up as a decision.
    if (opts.policy == RestorePolicy::Cost && engine) {
        const auto& kcal = opts.cold_cache ? cold_cal : warm_cal;
        double plan = 0.0, all_eager = 0.0;
        for (std::size_t i = 0; i < regions.size(); ++i) {
            const std::uint64_t bytes =
                static_cast<std::uint64_t>(regions[i].page_index_hi - regions[i].page_index_lo) *
                image.header().page_size;
            all_eager += kcal.cost_eager(bytes);
        }
        for (const auto& d : out.decisions) plan += d.predicted_cost;
        // Regions that were never offered to the engine are eager either way.
        for (const auto& r : regions)
            if (!uffd_eligible(r))
                plan += kcal.cost_eager(
                    static_cast<std::uint64_t>(r.page_index_hi - r.page_index_lo) *
                    image.header().page_size);
        if (all_eager - plan < kcal.deferral_setup_s) {
            std::fill(actions.begin(), actions.end(), paging::RestoreAction::Eager);
            out.decisions.clear();
            out.deferral_declined = true;
        }
    }

    const bool needs_uffd =
        std::any_of(actions.begin(), actions.end(), [](paging::RestoreAction a) {
            return a != paging::RestoreAction::Eager;
        });

    paging::EagerProvider eager(*target, image);
    std::shared_ptr<LazySession> session;
    if (needs_uffd) {
        session = std::make_shared<LazySession>();
        session->uffd = std::make_unique<Userfault>(*target);
        session->provider = std::make_unique<paging::PrefetchProvider>(image, *target,
                                                                      *session->uffd, history);
        session->provider->record_trace(!opts.trace_path.empty() || !opts.profile_path.empty());
    }

    for (std::size_t i = 0; i < regions.size(); ++i) {
        const auto id = static_cast<std::uint32_t>(i);
        if (actions[i] == paging::RestoreAction::Eager) {
            eager.prepare_region(id, paging::RestoreAction::Eager);
            ++out.regions_eager;
        } else {
            session->provider->prepare_region(id, actions[i]);
            ++out.regions_lazy;
        }
    }

    // §9.2: every saved page is now either written or covered by a handler.
    if (out.regions_eager + out.regions_lazy != regions.size())
        throw CheckpointError("restore: not every region was prepared");

    eager.apply_protections();
    // Verification reads pages back, which only makes sense where they are
    // already present; a deferred region is deliberately empty until it faults.
    if (opts.verify && opts.policy == RestorePolicy::Eager) eager.verify();
    out.pages = eager.stats();

    out.resume = resume(*target, image);
    out.resume_latency_ns = monotonic_ns() - started;
    out.pages_loaded_upfront = out.pages.pages_written + out.pages.pages_zeroed +
                               out.pages.pages_already_zero;
    out.actions = std::move(actions);
    if (session) {
        out.pages_loaded_upfront += session->provider->stats().pages_prefetched;
        out.lazy = session->provider->lazy_stats();
        out.prefetch = session->provider->stats();
        out.session = std::move(session);
    }
    return out;
}

}  // namespace adaptive_restore::restore
