#include "adaptive_restore/restore/restore.hpp"

#include <time.h>

#include <vector>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/paging/eager_provider.hpp"

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

RestoreSummary restore_image(const image::ImageReader& image, const RestoreOptions& opts) {
    const std::uint64_t started = monotonic_ns();

    RestoreSummary out;
    auto target = make_target_for(image.mappings(), opts.reserve_addr, opts.reserve_bytes);
    out.pid = target->pid();

    out.layout = rebuild_address_space(*target, image);

    // Decide an action per region. Phase 3's policy is fixed: private anonymous
    // regions go lazy, everything else stays eager. Phase 4 replaces this with
    // the cost model, which is the only reason regions exist.
    const auto regions = image.regions();
    const auto ranges = lazy_ranges(image);
    const auto is_lazy = [&ranges](const image::RegionRecord& r) {
        for (const auto& lr : ranges)
            if (r.start_vaddr >= lr.start && r.end_vaddr <= lr.end) return true;
        return false;
    };

    std::vector<paging::RestoreAction> actions(regions.size(), paging::RestoreAction::Eager);
    if (opts.policy == RestorePolicy::Lazy)
        for (std::size_t i = 0; i < regions.size(); ++i)
            if (is_lazy(regions[i])) actions[i] = paging::RestoreAction::Lazy;

    paging::EagerProvider eager(*target, image);
    std::shared_ptr<LazySession> session;
    if (opts.policy == RestorePolicy::Lazy) {
        session = std::make_shared<LazySession>();
        session->uffd = std::make_unique<Userfault>(*target);
        session->provider = std::make_unique<paging::LazyProvider>(image, *session->uffd);
    }

    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (actions[i] == paging::RestoreAction::Lazy) {
            session->provider->prepare_region(static_cast<std::uint32_t>(i),
                                              paging::RestoreAction::Lazy);
            ++out.regions_lazy;
        } else {
            eager.prepare_region(static_cast<std::uint32_t>(i), paging::RestoreAction::Eager);
            ++out.regions_eager;
        }
    }

    // §9.2: every saved page is now either written or covered by a handler.
    // Regions partition the page index (checked when the layout was rebuilt),
    // and each region got exactly one action, so the invariant holds -- but say
    // so out loud rather than leaving it implicit.
    if (out.regions_eager + out.regions_lazy != regions.size())
        throw CheckpointError("restore: not every region was prepared");

    eager.apply_protections();
    // Verification reads pages back, which only makes sense where they are
    // already present; a lazy region is deliberately empty until it faults.
    if (opts.verify && opts.policy == RestorePolicy::Eager) eager.verify();
    out.pages = eager.stats();

    out.resume = resume(*target, image);
    out.resume_latency_ns = monotonic_ns() - started;
    if (session) {
        out.lazy = session->provider->stats();
        out.session = std::move(session);
    }
    return out;
}

}  // namespace adaptive_restore::restore
