#include "adaptive_restore/restore/restore.hpp"

#include <time.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/common/version.hpp"
#include "adaptive_restore/paging/eager_provider.hpp"
#include "adaptive_restore/policy/plan.hpp"
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

}  // namespace

RestoreSummary restore_image(const image::ImageReader& image, const RestoreOptions& opts) {
    const std::uint64_t started = monotonic_ns();

    RestoreSummary out;
    auto target = make_target_for(image.mappings(), opts.reserve_addr, opts.reserve_bytes);
    out.pid = target->pid();

    out.layout = rebuild_address_space(*target, image);

    const auto regions = image.regions();
    const auto mappings = image.mappings();
    const auto& history = opts.history;  // loaded by the caller, before the clock

    // --- decide an action per region ----------------------------------------
    // The whole plan comes from the policy layer, so `arctl visualize` can draw
    // exactly what a restore would do without re-deriving any of it.
    const auto plan = policy::plan_restore(image, history, opts.policy, opts.cold_cache);
    auto actions = plan.actions;
    out.decisions = plan.decisions;
    out.predicted_transfer_s = plan.predicted_transfer_s;
    out.deferral_declined = plan.deferral_declined;

    // Runs AFTER the decline gate above: the gate can revert every action to
    // Eager, and disabling THP for a plan that is then discarded leaves the
    // target paying the penalty for deferral it never performs.
    //
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

    out.resume = resume(*target, image, session ? session->uffd->target_fd() : -1);
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
