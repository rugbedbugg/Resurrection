#include <sys/wait.h>

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
    } else {
        return fail("policy '" + policy +
                    "' is not implemented yet; available policies are eager and lazy");
    }

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
                  << " lazy\n"
                  << "  verify     "
                  << (!opts.verify ? "skipped"
                      : opts.policy == restore::RestorePolicy::Eager
                          ? "0 mismatches"
                          : "eager regions only (lazy pages are not resident yet)")
                  << '\n'
                  << "  resume     " << ms(s.resume_latency_ns) << " to first instruction\n";

        // Under lazy the target is running with pages still owed to it, so
        // arctl becomes its page server for the rest of its life.
        int st = 0;
        bool reaped = false;
        if (s.session) {
            std::cout << "  serving    " << s.lazy.pages_registered << " pages across "
                      << s.lazy.regions_registered << " registered regions\n";
            const auto served = s.session->serve(s.pid);
            const auto& ls = s.session->provider->stats();
            std::cout << "  faults     " << served.faults_served << " served ("
                      << ls.copy_faults << " copy, " << ls.zero_faults << " zero), "
                      << ls.bytes_supplied << " bytes\n";
            if (ls.pages_registered > 0)
                std::cout << "  untouched  " << (ls.pages_registered - served.faults_served)
                          << " of " << ls.pages_registered
                          << " registered pages were never demanded\n";
            if (ls.faults_served > 0)
                std::cout << "  latency    " << (ls.service_ns_total / ls.faults_served)
                          << " ns mean per fault, " << ls.service_ns_total << " ns total\n";
            if (!opts.trace_path.empty()) {
                s.session->provider->write_trace(opts.trace_path);
                std::cout << "  trace      " << s.session->provider->trace().size()
                          << " faults written to " << opts.trace_path << '\n';
            }
#ifdef AR_HAVE_SQLITE
            // A lazy restore is the only kind that can teach: every first touch
            // faults, so its trace is the touched set.
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
