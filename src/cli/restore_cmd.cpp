#include <sys/wait.h>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <unordered_set>
#include <cstdlib>
#include <iostream>
#include <string>

#include "adaptive_restore/cli/commands.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/common/version.hpp"
#include "adaptive_restore/restore/restore.hpp"
#ifdef AR_HAVE_SQLITE
#include "adaptive_restore/policy/metrics.hpp"
#include "adaptive_restore/profile/profile_store.hpp"
#include "adaptive_restore/profile/region_profiler.hpp"
#endif

namespace adaptive_restore::cli {

namespace {

int fail(const std::string& msg) {
    std::cerr << "arctl restore: " << msg << '\n';
    return 1;
}

std::string ms(std::uint64_t ns) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f ms", static_cast<double>(ns) / 1e6);
    return std::string(b);
}

}  // namespace

int cmd_restore(std::span<const std::string_view> args) {
    std::string image_path;
    std::string policy = "eager";
    restore::RestoreOptions opts;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--verify") {
            opts.verify = true;
        } else if (a == "--no-learn") {
            // Bookkeeping is not restore work. Counting a profile write inside
            // a policy comparison charges the adaptive policies for something
            // the fixed ones never do.
            opts.learn = false;
        } else if (a == "--cold-cache") {
            opts.cold_cache = true;
        } else if (a == "--warm-cache") {
            opts.cold_cache = false;
        } else if (a == "--image" || a == "--policy" || a == "--record-trace" ||
                   a == "--profile") {
            if (i + 1 >= args.size()) return fail(std::string(a) + " needs a value");
            const std::string value(args[++i]);
            if (a == "--image")
                image_path = value;
            else if (a == "--policy")
                policy = value;
            else if (a == "--record-trace")
                opts.trace_path = value;
            else
                opts.profile_path = value;
        } else {
            return fail("unknown option '" + std::string(a) + "'");
        }
    }

    if (image_path.empty())
        return fail("usage: arctl restore --image <image>\n"
                    "         [--policy eager|lazy|prefetch|cost]  (default eager;\n"
                    "          prefetch and cost also need --profile)\n"
                    "         [--profile <db>] [--verify] [--record-trace <path>]\n"
                    "         [--cold-cache|--warm-cache] [--no-learn]");
    if (policy == "eager") {
        opts.policy = restore::RestorePolicy::Eager;
    } else if (policy == "lazy") {
        opts.policy = restore::RestorePolicy::Lazy;
    } else if (policy == "prefetch") {
        opts.policy = restore::RestorePolicy::Prefetch;
    } else if (policy == "cost") {
        opts.policy = restore::RestorePolicy::Cost;
    } else {
        return fail("policy '" + policy +
                    "' is not recognised; available policies are eager, lazy, prefetch and cost");
    }
#ifndef AR_HAVE_SQLITE
    if (opts.policy == restore::RestorePolicy::Prefetch ||
        opts.policy == restore::RestorePolicy::Cost)
        return fail("policy '" + policy +
                    "' needs the profile store; this build was configured without SQLite");
#endif
    if ((opts.policy == restore::RestorePolicy::Prefetch ||
         opts.policy == restore::RestorePolicy::Cost) &&
        opts.profile_path.empty())
        return fail("policy '" + policy + "' needs --profile <db> to read region history");

    try {
        const auto image = image::ImageReader::open(image_path);

#ifdef AR_HAVE_SQLITE
        // Read the profile here, alongside the image, so both inputs are loaded
        // on the same side of the restore clock.
        profile::ProfileKey pkey;
        pkey.image_digest = image.identity_digest();
        pkey.region_layout_version = kRegionLayoutVersion;
        pkey.feature_schema_version = kFeatureSchemaVersion;
        if (!opts.profile_path.empty()) {
            auto store = profile::ProfileStore::open(opts.profile_path);
            opts.history = store.load(pkey);
        }
#endif

        const auto s = restore::restore_image(image, opts);

        std::cout << "restored " << image_path << " as pid " << s.pid << '\n'
                  << "  mappings   " << s.layout.created << " recreated, "
                  << s.layout.skipped_kernel << " kernel-provided\n"
                  << "  pages      " << s.pages.pages_written << " written, "
                  << (s.pages.pages_zeroed + s.pages.pages_already_zero) << " zero\n"
                  << "  page data  " << s.pages.bytes_written << " bytes in "
                  << s.pages.writev_calls << " writes\n"
                  << "  fds        " << s.resume.fds_restored << " restored\n"
                  << "  regions    " << s.regions_eager << " eager, " << s.regions_lazy
                  << " deferred\n"
                  << "  verify     "
                  << (!opts.verify ? "skipped"
                      : opts.policy == restore::RestorePolicy::Eager
                          ? "0 mismatches"
                          : "eager regions only (lazy pages are not resident yet)")
                  << '\n'
                  << "  resume     " << ms(s.resume_latency_ns) << " to first instruction\n";

#ifdef AR_HAVE_SQLITE
        // The oracle: the cheapest any policy could be with perfect per-region
        // knowledge. Without it a win over the baselines is unanchored -- it
        // could be most of the available headroom or a sliver of it.
        if (!opts.profile_path.empty()) {
            const auto& hist = opts.history;
            if (!hist.empty()) {
                const auto bound = policy::oracle_bound(
                    image, hist,
                    opts.cold_cache ? policy::Calibration::defaults_cold()
                                    : policy::Calibration::defaults_warm());
                std::cout << "  oracle     " << ms(static_cast<std::uint64_t>(bound.seconds * 1e9))
                          << " transfer bound (" << bound.regions_eager << " eager, "
                          << bound.regions_prefetch << " prefetch, " << bound.regions_lazy
                          << " lazy with perfect knowledge)\n";
                // Score the chosen actions against ground truth, not against
                // the predictions that produced them: a controller's own
                // estimate is built from averaged history and can be
                // optimistic, which would let a plan appear to beat the bound.
                if (!s.actions.empty() && bound.seconds > 0) {
                    const double realised = policy::plan_cost(
                        image, hist, s.actions,
                        opts.cold_cache ? policy::Calibration::defaults_cold()
                                        : policy::Calibration::defaults_warm(),
                        0.0);
                    char pct[32];
                    std::snprintf(pct, sizeof pct, "%.1f", 100.0 * bound.seconds / realised);
                    std::cout << "  plan       "
                              << ms(static_cast<std::uint64_t>(realised * 1e9))
                              << " scored against ground truth, capturing " << pct
                              << "% of the achievable gain\n";
                }
            }
        }
#endif

        // This controller is meant to be deterministic and explainable, so
        // the decisions travel with their costs. A breakdown nobody can read
        // fails that claim on its own terms.
        if (s.deferral_declined)
            std::cout << "  decisions  deferred nothing: the plan did not save more than the "
                         "cost of standing up a page server\n";
        if (!s.decisions.empty()) {
            std::size_t n_eager = 0, n_pref = 0, n_lazy = 0;
            for (const auto& d : s.decisions) {
                if (d.action == paging::RestoreAction::Eager) ++n_eager;
                else if (d.action == paging::RestoreAction::StaticPrefetch) ++n_pref;
                else ++n_lazy;
            }
            // The cost model only ever sees uffd-eligible regions; the rest are
            // forced eager before it runs. Without this reconciliation the two
            // lines look like they disagree (90 eager above vs 68 here).
            const std::size_t eligible = s.decisions.size();
            const std::size_t forced =
                s.actions.size() > eligible ? s.actions.size() - eligible : 0;
            std::cout << "  decisions  " << n_eager << " eager, " << n_pref << " prefetch, "
                      << n_lazy << " lazy  (" << (opts.cold_cache ? "cold" : "warm")
                      << "-cache calibration)\n";
            if (forced > 0)
                std::cout << "             of " << eligible << " eligible regions; " << forced
                          << " more forced eager, so " << (n_eager + forced) << " / "
                          << (n_pref + n_lazy) << " above\n";
            std::cout << "             region     chose   cost_eager  cost_prefetch"
                         "   cost_lazy   conf\n";
            std::size_t shown = 0;
            for (const auto& d : s.decisions) {
                if (shown++ >= 6) break;
                const char* name = d.action == paging::RestoreAction::Eager      ? "eager"
                                   : d.action == paging::RestoreAction::StaticPrefetch
                                       ? "prefetch"
                                       : "lazy";
                // An infinite prefetch cost means "nothing recorded to load",
                // which is a real state and prints as a dash, not a number.
                char pf[24];
                if (std::isinf(d.cost_prefetch))
                    std::snprintf(pf, sizeof pf, "%13s", "-");
                else
                    std::snprintf(pf, sizeof pf, "%11.1fus", d.cost_prefetch * 1e6);

                char line[160];
                std::snprintf(line, sizeof line, "             %6u  %8s  %9.1fus %s  %8.1fus  %.2f\n",
                              d.region_id, name, d.cost_eager * 1e6, pf, d.cost_lazy * 1e6,
                              static_cast<double>(d.confidence));
                std::cout << line;
            }
            if (s.decisions.size() > 6)
                std::cout << "             ... " << (s.decisions.size() - 6) << " more\n";
        }

        // Under a deferring policy the target is running with pages still owed
        // to it, so arctl becomes its page server for the rest of its life.
        int st = 0;
        bool reaped = false;
        if (s.session) {
            const auto& ls = s.session->provider->lazy_stats();
            const auto& ps = s.session->provider->stats();
            std::cout << "  deferred   " << ls.pages_registered << " pages across "
                      << ls.regions_registered << " registered regions\n";
            if (ps.pages_prefetched > 0)
                std::cout << "  prefetched " << ps.pages_prefetched << " pages ("
                          << ps.bytes_prefetched << " bytes) from history across "
                          << ps.regions_prefetched << " regions\n";
            if (ps.regions_without_history > 0)
                std::cout << "  no history " << ps.regions_without_history
                          << " regions degraded to pure lazy\n";

            const auto served = s.session->serve(s.pid);
            std::cout << "  faults     " << served.faults_served << " served ("
                      << ls.copy_faults << " copy, " << ls.zero_faults << " zero), "
                      << ls.bytes_supplied << " bytes\n";
            if (ls.pages_registered > 0) {
                // Neither loaded up front nor demanded: genuinely untouched.
                // Subtracting only the faults would count prefetched pages as
                // untouched, which is exactly backwards.
                const auto accounted = served.faults_served + ps.pages_prefetched;
                const auto untouched =
                    ls.pages_registered > accounted ? ls.pages_registered - accounted : 0;
                std::cout << "  untouched  " << untouched << " of " << ls.pages_registered
                          << " deferred pages were neither loaded nor demanded\n";
            }
            if (ls.faults_served > 0)
                std::cout << "  latency    " << (ls.service_ns_total / ls.faults_served)
                          << " ns mean per fault, " << ls.service_ns_total << " ns total\n";
            // EXPERIMENTAL: dump every page the prediction
            // named, flagged with whether it was withheld. Off unless the env
            // var is set, so no normal restore writes this.
            if (const char* rp = std::getenv("AR_EXPERIMENTAL_HOLDOUT_REPORT")) {
                const auto regions = image.regions();
                const auto pidx = image.page_index();
                const auto& ex = s.executions;
                std::unordered_set<std::uint64_t> held;
                for (const auto& h : s.session->provider->holdouts()) held.insert(h.vaddr);
                std::unordered_set<std::uint64_t> faulted;
                for (const auto& f : s.session->provider->trace()) faulted.insert(f.vaddr);

                std::ofstream f(rp);
                f << "region\tpage\tvaddr\tpredicted\theld_out\tfaulted\n";
                for (std::size_t i = 0; i < regions.size() && i < ex.size(); ++i) {
                    const auto& r = regions[i];
                    const std::uint32_t n = r.page_index_hi - r.page_index_lo;
                    for (std::uint32_t p = 0; p < n; ++p) {
                        const std::uint32_t gi = r.page_index_lo + p;
                        if (gi >= pidx.size()) continue;
                        const std::uint64_t va = pidx[gi].vaddr;
                        const bool wrote = ex[i].populated_page(p);
                        const bool out = held.count(va) > 0;
                        if (!wrote && !out) continue;   // not part of the prediction
                        f << r.region_id << '\t' << p << '\t' << va << '\t' << 1 << '\t'
                          << (out ? 1 : 0) << '\t' << (faulted.count(va) ? 1 : 0) << '\n';
                    }
                }
                std::cout << "  holdout    report written to " << rp << " (k="
                          << s.session->provider->stats().pages_held_out << " pages withheld)\n";
            }

            if (!opts.trace_path.empty()) {
                s.session->provider->write_trace_to(opts.trace_path);
                std::cout << "  trace      " << s.session->provider->trace().size()
                          << " faults written to " << opts.trace_path << '\n';
            }
#ifdef AR_HAVE_SQLITE
            // Only regions left *pure lazy* teach. A page written up front never
            // faults, so nothing observes it -- and that includes the pages a
            // prefetch region loaded from history, which is why a --policy
            // prefetch run records nothing at all and a --policy cost run
            // records only its lazy regions. See region_profiler.hpp.
            if (!opts.profile_path.empty() && opts.learn) {
                const auto observed = profile::observe_regions(
                    image, s.session->provider->trace(), s.executions);
                std::uint32_t partial = 0;
                const auto history = profile::to_history(observed, &partial);
                profile::ProfileKey key;
                key.image_digest = image.identity_digest();
                key.region_layout_version = kRegionLayoutVersion;
                key.feature_schema_version = kFeatureSchemaVersion;
                auto store = profile::ProfileStore::open(opts.profile_path);
                store.record(key, history);
                std::cout << "  observed   " << observed.size()
                          << " regions witnessed evidence\n"
                          << "  profile    " << history.size() << " regions recorded ("
                          << store.restores_recorded(key) << " restores learned)\n";
                if (partial > 0)
                    std::cout << "             " << partial
                              << " partially observed regions not stored: v2 history cannot\n"
                                 "             mark a page unknown, and a resident page is "
                                 "unknown, not untouched\n";
            }
#endif
            st = served.exit_status;
            reaped = served.target_exited;
        }
        if (!reaped) reaped = (::waitpid(s.pid, &st, 0) == s.pid);
        if (reaped) {
            if (WIFEXITED(st))
                std::cout << "  target     exited " << WEXITSTATUS(st) << '\n';
            else if (WIFSIGNALED(st))
                std::cout << "  target     killed by signal " << WTERMSIG(st) << '\n';
        }
    } catch (const std::exception& e) {
        return fail(e.what());
    }
    return 0;
}

}  // namespace adaptive_restore::cli
