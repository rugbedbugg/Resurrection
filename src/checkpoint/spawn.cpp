#include "adaptive_restore/checkpoint/spawn.hpp"

#include <fcntl.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/common/sha256.hpp"

namespace adaptive_restore::checkpoint {

namespace {

// Written by the child if exec fails, so the parent can report the real errno
// instead of a bare exit status 127. Closed by exec on success, giving EOF.
struct SyncPipe {
    int fds[2] = {-1, -1};
    ~SyncPipe() {
        if (fds[0] >= 0) ::close(fds[0]);
        if (fds[1] >= 0) ::close(fds[1]);
    }
};

// Resume a stopped tracee, forwarding `sig` (0 suppresses it).
void cont_or_throw(pid_t pid, int sig, const char* where) {
    if (::ptrace(PTRACE_CONT, pid, nullptr, reinterpret_cast<void*>(
                                                static_cast<std::intptr_t>(sig))) != 0)
        throw CheckpointError(std::string("spawn: PTRACE_CONT (") + where +
                              "): " + std::strerror(errno));
}

void kill_and_reap(pid_t pid) {
    ::kill(pid, SIGKILL);
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
}

}  // namespace

std::vector<std::string> build_env(const EnvPolicy& policy) {
    std::vector<std::string> env;
    for (const std::string& name : policy.passthrough_names)
        if (const char* value = ::getenv(name.c_str())) env.push_back(name + "=" + value);
    for (const std::string& entry : policy.extra) env.push_back(entry);
    return env;
}

std::array<std::byte, 32> launch_digest(const std::vector<std::string>& argv,
                                        const std::vector<std::string>& envp) {
    std::vector<std::byte> buf;
    const auto append = [&buf](const std::vector<std::string>& v) {
        for (const std::string& s : v) {
            const auto* p = reinterpret_cast<const std::byte*>(s.data());
            buf.insert(buf.end(), p, p + s.size());
            buf.push_back(std::byte{0});
        }
    };
    append(argv);
    append(envp);
    return sha256(buf);
}

SpawnedTarget spawn_to_marker(const std::vector<std::string>& argv, const EnvPolicy& policy,
                              int stdout_fd, int stderr_fd) {
    if (argv.empty()) throw CheckpointError("spawn: empty argv");

    const std::vector<std::string> envp = build_env(policy);

    // Build the C arrays before forking: the child must not allocate.
    std::vector<char*> argv_c;
    argv_c.reserve(argv.size() + 1);
    for (const std::string& a : argv) argv_c.push_back(const_cast<char*>(a.c_str()));
    argv_c.push_back(nullptr);

    std::vector<char*> envp_c;
    envp_c.reserve(envp.size() + 1);
    for (const std::string& e : envp) envp_c.push_back(const_cast<char*>(e.c_str()));
    envp_c.push_back(nullptr);

    SyncPipe sync;
    if (::pipe2(sync.fds, O_CLOEXEC) != 0)
        throw CheckpointError(std::string("spawn: pipe2: ") + std::strerror(errno));

    const pid_t pid = ::fork();
    if (pid < 0) throw CheckpointError(std::string("spawn: fork: ") + std::strerror(errno));

    if (pid == 0) {
        // --- child ---------------------------------------------------------
        ::close(sync.fds[0]);
        const int err_fd = sync.fds[1];
        const auto die = [err_fd](int e) {
            ssize_t ignored = ::write(err_fd, &e, sizeof e);
            (void)ignored;
            _exit(127);
        };

        // Saved absolute addresses only reproduce with layout randomisation off.
        if (::personality(ADDR_NO_RANDOMIZE) == -1) die(errno);
        if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0) die(errno);
        if (stdout_fd >= 0 && ::dup2(stdout_fd, STDOUT_FILENO) < 0) die(errno);
        if (stderr_fd >= 0 && ::dup2(stderr_fd, STDERR_FILENO) < 0) die(errno);

        // Stop so the parent can install ptrace options before exec runs.
        ::raise(SIGSTOP);

        ::execvpe(argv_c[0], argv_c.data(), envp_c.data());
        die(errno);  // only reached if exec failed
    }

    // --- parent -------------------------------------------------------------
    ::close(sync.fds[1]);
    sync.fds[1] = -1;

    const auto read_child_errno = [&sync]() -> int {
        int e = 0;
        const ssize_t n = ::read(sync.fds[0], &e, sizeof e);
        return (n == static_cast<ssize_t>(sizeof e)) ? e : 0;
    };

    try {
        int st = 0;
        while (::waitpid(pid, &st, 0) < 0) {
            if (errno != EINTR)
                throw CheckpointError(std::string("spawn: waitpid: ") + std::strerror(errno));
        }
        if (!WIFSTOPPED(st) || WSTOPSIG(st) != SIGSTOP) {
            const int e = read_child_errno();
            throw CheckpointError("spawn: target failed before exec" +
                                  (e ? std::string(": ") + std::strerror(e) : std::string()));
        }

        // EXITKILL stops a crashed arctl from leaving the target stopped forever.
        if (::ptrace(PTRACE_SETOPTIONS, pid, nullptr,
                     reinterpret_cast<void*>(static_cast<std::intptr_t>(
                         PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD))) != 0)
            throw CheckpointError(std::string("spawn: PTRACE_SETOPTIONS: ") +
                                  std::strerror(errno));
        cont_or_throw(pid, 0, "pre-exec");

        for (;;) {
            while (::waitpid(pid, &st, 0) < 0) {
                if (errno != EINTR)
                    throw CheckpointError(std::string("spawn: waitpid: ") + std::strerror(errno));
            }

            if (WIFEXITED(st) || WIFSIGNALED(st)) {
                const int e = read_child_errno();
                if (e)
                    throw CheckpointError("spawn: cannot execute \"" + argv[0] +
                                          "\": " + std::strerror(e));
                throw CheckpointError("spawn: target exited before reaching "
                                      "ar_checkpoint_here()");
            }
            if (!WIFSTOPPED(st))
                throw CheckpointError("spawn: unexpected wait status from the target");

            const int sig = WSTOPSIG(st);
            if (sig == SIGSTOP) break;  // the marker

            // SIGTRAP is the kernel's post-exec notification, not a real signal,
            // so it is swallowed. Anything else is the target's own signal and
            // is re-injected so its behaviour is unchanged by being traced.
            cont_or_throw(pid, (sig == SIGTRAP) ? 0 : sig, "run to marker");
        }
    } catch (...) {
        kill_and_reap(pid);
        throw;
    }

    return SpawnedTarget{pid, argv, envp};
}

}  // namespace adaptive_restore::checkpoint
