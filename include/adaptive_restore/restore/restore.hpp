#pragma once
#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "adaptive_restore/restore/address_space.hpp"
#include "adaptive_restore/restore/page_writer.hpp"
#include "adaptive_restore/paging/prefetch_provider.hpp"
#include "adaptive_restore/policy/region.hpp"
#include "adaptive_restore/restore/resume.hpp"
#include "adaptive_restore/restore/uffd.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::restore {

enum class RestorePolicy {
    Eager,     // every page written before the target resumes (§9.3)
    Lazy,      // anonymous regions registered and served on demand (§9.4)
    Prefetch,  // load the recorded working set, fault for the rest (§9.5)
    Cost,      // choose per region from the cost model (§12)
};

struct RestoreOptions {
    bool verify = false;
    RestorePolicy policy = RestorePolicy::Eager;
    std::string trace_path;    // when set, the demand-fault trace is written here
    std::string profile_path;  // when set, a restore reads and teaches this profile
    bool cold_cache = true;    // calibration to use; see the calibration report
    bool learn = true;         // false: read the profile but do not write it back
    // Scratch area for compel. Kept far from any realistic process layout so it
    // cannot collide with an image; see docs/phase2-spike-report.md.
    std::uint64_t reserve_addr = 0x700000000000ULL;
    std::size_t reserve_bytes = 4 * 4096;
};

struct LazySession;

struct RestoreSummary {
    pid_t pid = -1;
    LayoutResult layout;
    PopulateResult pages;
    ResumeResult resume;
    paging::LazyStats lazy;               // zero under RestorePolicy::Eager
    paging::PrefetchStats prefetch;       // zero unless a policy prefetched
    std::vector<policy::RegionDecision> decisions;  // populated under Cost
    std::vector<paging::RestoreAction> actions;  // what each region was prepared with
    std::uint64_t regions_eager = 0;
    std::uint64_t regions_lazy = 0;
    std::uint64_t resume_latency_ns = 0;  // invocation to first resumed instruction (§22)

    // Under Lazy the target is running with pages still owed to it, so the
    // caller MUST drive serve() until it exits. Null under Eager.
    std::shared_ptr<LazySession> session;
};

// Everything a lazy restore needs to keep serving after control transfer. The
// uffd and the provider must outlive the call, because the restored process is
// running with pages still owed to it.
struct LazySession {
    std::unique_ptr<Userfault> uffd;
    std::unique_ptr<paging::PrefetchProvider> provider;

    // Serve demand faults until the target exits. Kills the target rather than
    // returning if a fault cannot be resolved.
    paging::ServeResult serve(pid_t target_pid);
};

// Rebuild the image into a fresh process and hand it control. Returns once the
// process is running. Under Eager the caller just waits on it; under Lazy the
// caller must call summary.session->serve(pid) or the target stalls on its
// first fault. Throws on any failure before control transfer, leaving no
// partially-restored process behind.
RestoreSummary restore_image(const image::ImageReader& image, const RestoreOptions& opts);

}  // namespace adaptive_restore::restore
