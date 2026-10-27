#pragma once
#include <sys/types.h>

#include <cstdint>
#include <vector>

struct parasite_ctl;

namespace adaptive_restore::restore {

// A freshly forked process, seized by compel, into which an image is rebuilt.
//
// The stub deliberately does NOT ptrace(PTRACE_TRACEME): compel attaches with
// PTRACE_SEIZE, which fails EPERM on a task that already has a tracer. So the
// stub only signals readiness and idles; compel owns the whole attachment.
class RestoreTarget {
public:
    // Fork an idle stub with ASLR off and a sanitised fd table, then seize it.
    // `reserve_addr` is where the scratch area is mapped: it must not collide
    // with any mapping the image will claim. Throws CheckpointError on failure.
    explicit RestoreTarget(std::uint64_t reserve_addr, std::size_t reserve_bytes);

    ~RestoreTarget();
    RestoreTarget(const RestoreTarget&) = delete;
    RestoreTarget& operator=(const RestoreTarget&) = delete;

    pid_t pid() const { return pid_; }
    std::uint64_t reserve_addr() const { return reserve_addr_; }
    std::size_t reserve_bytes() const { return reserve_bytes_; }
    parasite_ctl* ctl() const { return ctl_; }

    // Run one syscall inside the target. Throws on injection failure; returns
    // the target's raw syscall result (negative errno on syscall failure).
    long syscall(int nr, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0,
                 std::uint64_t a4 = 0, std::uint64_t a5 = 0, std::uint64_t a6 = 0);

    // Base of the VMA holding the stub's TLS. That block is load-bearing for
    // compel and cannot be relocated, so an image mapping overlapping it makes
    // this stub unusable.
    std::uint64_t tls_start() const { return tls_start_; }
    std::uint64_t tls_end() const { return tls_end_; }

    // Hand control to `pid_` and stop managing it. After this the target is no
    // longer ours and the destructor will not kill it.
    void release() { released_ = true; }

private:
    void kill_and_reap();

    pid_t pid_ = -1;
    parasite_ctl* ctl_ = nullptr;
    std::uint64_t reserve_addr_ = 0;
    std::size_t reserve_bytes_ = 0;
    std::uint64_t tls_start_ = 0, tls_end_ = 0;
    bool released_ = false;
};

}  // namespace adaptive_restore::restore
