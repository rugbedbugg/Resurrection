#include <sys/wait.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

#include "adaptive_restore/cli/commands.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/common/version.hpp"
#include "adaptive_restore/restore/restore.hpp"
#ifdef AR_HAVE_SQLITE
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
        return fail("usage: arctl restore --image <image> --policy eager|lazy "
                    "[--verify] [--record-trace <path>]");
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

        // §12 sells this controller on being deterministic and explainable, so
        // the decisions travel with their costs. A breakdown nobody can read
        // fails that claim on its own terms.
        if (!s.decisions.empty()) {
            std::size_t n_eager = 0, n_pref = 0, n_lazy = 0;
            for (const auto& d : s.decisions) {
                if (d.action == paging::RestoreAction::Eager) ++n_eager;
                else if (d.action == paging::RestoreAction::StaticPrefetch) ++n_pref;
                else ++n_lazy;
            }
            std::cout << "  decisions  " << n_eager << " eager, " << n_pref << " prefetch, "
                      << n_lazy << " lazy  (" << (opts.cold_cache ? "cold" : "warm")
                      << "-cache calibration)\n";
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
            if (!opts.trace_path.empty()) {
                s.session->provider->write_trace_to(opts.trace_path);
                std::cout << "  trace      " << s.session->provider->trace().size()
                          << " faults written to " << opts.trace_path << '\n';
            }
#ifdef AR_HAVE_SQLITE
            // Only a restore that deferred pages can teach: a page that was
            // written up front never faults, so nothing observes it.
            if (!opts.profile_path.empty()) {
                const auto observed = profile::observe_regions(
                    image, s.session->provider->trace(), s.actions);
                profile::ProfileKey key;
                key.image_digest = image.identity_digest();
                key.region_layout_version = kRegionLayoutVersion;
                key.feature_schema_version = kFeatureSchemaVersion;
                auto store = profile::ProfileStore::open(opts.profile_path);
                store.record(key, observed);
                std::cout << "  profile    " << observed.size() << " regions recorded ("
                          << store.restores_recorded(key) << " restores learned)\n";
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
