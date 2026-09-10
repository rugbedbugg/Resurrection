#pragma once
#include <sys/types.h>

#include <string>
#include <vector>

namespace adaptive_restore::checkpoint {

// State a Tier 1 checkpoint neither captures nor reconstructs.
//
// The fd collector already refuses what it cannot represent, loudly, naming the
// descriptor. This closes the same gap for everything else: signal dispositions
// and masks, threads beyond the first, seccomp, the working directory, umask.
// None of it is in the image format, so a restored process silently inherits
// arctl's instead of the original's. Silent divergence is the worst failure
// mode available -- the workload produces a plausible wrong answer, and the
// cause is nowhere near the symptom.
//
// Only *non-default* values are refused. A process that blocked no signals,
// installed no handlers and never chdir'd loses nothing by not having them
// captured, which is why `mini` and the benchmark workloads are unaffected.
struct UnsupportedState {
    std::vector<std::string> reasons;  // empty means nothing was found

    bool empty() const { return reasons.empty(); }
    std::string describe() const;
};

// Inspect a stopped target. `arctl_cwd` is compared against the target's, since
// a restored process inherits arctl's working directory.
UnsupportedState find_unsupported_state(pid_t pid);

}  // namespace adaptive_restore::checkpoint
