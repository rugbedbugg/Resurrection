#include "adaptive_restore/restore/restore.hpp"

#include <time.h>

#include <algorithm>
#include <memory>
#include <unordered_map>
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

    std::vector<profile::RegionHistory> history;
#ifdef AR_HAVE_SQLITE
    profile::ProfileKey key;
    key.image_digest = image.identity_digest();
    key.region_layout_version = kRegionLayoutVersion;
    key.feature_schema_version = kFeatureSchemaVersion;
    if (!opts.profile_path.empty()) {
        auto store = profile::ProfileStore::open(opts.profile_path);
        history = store.load(key);
    }
#endif
    std::unordered_map<std::uint32_t, const profile::RegionHistory*> by_id;
    for (const auto& h : history) by_id[h.region_id] = &h;

    // --- decide an action per region ----------------------------------------
    std::vector<paging::RestoreAction> actions(regions.size(), paging::RestoreAction::Eager);
    std::unique_ptr<policy::CostModelPolicyEngine> engine;
    if (opts.policy == RestorePolicy::Cost)
        engine = std::make_unique<policy::CostModelPolicyEngine>(
            policy::Calibration::defaults_cold(), policy::Calibration::defaults_warm());

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
