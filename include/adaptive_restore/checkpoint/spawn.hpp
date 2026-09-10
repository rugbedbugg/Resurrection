#pragma once
#include <sys/types.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace adaptive_restore::checkpoint {

// Which environment variables the target is allowed to see. The environment
// shapes the initial stack, so saved absolute addresses only reproduce if it is
// curated rather than inherited wholesale.
struct EnvPolicy {
    std::vector<std::string> passthrough_names{"PATH", "LANG"};
    std::vector<std::string> extra;  // literal "KEY=VALUE" entries
};

// A target stopped at ar_checkpoint_here().
//
// argv/envp are the exact vectors handed to the child. They are carried here
// because launch_digest() must hash what the process actually launched with,
// and the caller cannot otherwise reconstruct the curated environment.
struct SpawnedTarget {
    pid_t pid = -1;
    std::vector<std::string> argv;
    std::vector<std::string> envp;
};

// Resolve an EnvPolicy against the current environment.
std::vector<std::string> build_env(const EnvPolicy& policy);

// SHA-256 over argv[0]\0...argv[n]\0envp[0]\0...envp[n]\0, stored in the image
// header so a restore can detect that the launch context changed.
std::array<std::byte, 32> launch_digest(const std::vector<std::string>& argv,
                                        const std::vector<std::string>& envp);

// Fork/exec argv under ptrace with ASLR disabled and a curated environment, then
// run it until it stops at the marker. Returns with the target ptrace-stopped.
// Throws CheckpointError if the target cannot be started or exits first.
//
// The child gets a fully determined descriptor table: stdin/stdout/stderr are
// pointed at the given fds (/dev/null for any that is -1) and every inherited
// descriptor above 2 is closed. Without this the target would inherit whatever
// arctl was launched with -- a terminal, a pipe, a socket -- which both makes
// the capture environment-dependent and hands collect_fds() descriptors that
// Tier 1 cannot represent.
SpawnedTarget spawn_to_marker(const std::vector<std::string>& argv, const EnvPolicy& policy,
                              int stdout_fd, int stderr_fd, int stdin_fd = -1);

}  // namespace adaptive_restore::checkpoint
