#include "adaptive_restore/restore/target.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <elf.h>
#include <string>

#include "adaptive_restore/checkpoint/maps.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/restore/compel_compat.hpp"

namespace adaptive_restore::restore {

namespace {

// compel talks to /proc on the target's behalf. compel_prepare_noctx() leaves
// this unset, and an unset open_proc is a null deref waiting to happen.
int target_open_proc(int pid, int mode, const char* fmt, ...) {
    char tail[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(tail, sizeof tail, fmt, ap);
    va_end(ap);

    char path[320];
    std::snprintf(path, sizeof path, "/proc/%d/%s", pid, tail);
    return ::open(path, mode);
}

// compel logs through a callback; without one it is silent, which makes every
// failure look identical. Route it at our own stderr only when it matters.
void compel_sink(unsigned int level, const char* fmt, va_list args) {
    if (level <= COMPEL_LOG_ERROR) std::vfprintf(stderr, fmt, args);
}

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}

void close_inherited_fds() {
    if (::close_range(3, ~0U, 0) == 0) return;
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
    for (int fd = 3; fd < static_cast<int>(max_fd); ++fd) ::close(fd);
}

}  // namespace

RestoreTarget::RestoreTarget(std::uint64_t reserve_addr, std::size_t reserve_bytes)
    : reserve_addr_(reserve_addr), reserve_bytes_(reserve_bytes) {
    compel_log_init(compel_sink, COMPEL_LOG_ERROR);

    int tube[2];
    if (::pipe2(tube, O_CLOEXEC) != 0)
        throw CheckpointError(std::string("restore: pipe2: ") + std::strerror(errno));

    pid_ = ::fork();
    if (pid_ < 0) {
        ::close(tube[0]);
        ::close(tube[1]);
        throw CheckpointError(std::string("restore: fork: ") + std::strerror(errno));
    }

    if (pid_ == 0) {
        // --- the stub. No PTRACE_TRACEME: compel does the seizing. ---
        ::close(tube[0]);
        if (::personality(ADDR_NO_RANDOMIZE) == -1) _exit(70);

        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd < 0) _exit(71);
        for (int i = 0; i < 3; ++i)
            if (null_fd != i && ::dup2(null_fd, i) < 0) _exit(72);

        const char ready = 1;
        if (::write(tube[1], &ready, 1) != 1) _exit(73);
        ::close(tube[1]);
        close_inherited_fds();

        for (;;) ::pause();
        _exit(74);
    }

    ::close(tube[1]);
    char ready = 0;
    const ssize_t n = ::read(tube[0], &ready, 1);
    ::close(tube[0]);
    if (n != 1) {
        kill_and_reap();
        throw CheckpointError("restore: stub died before signalling readiness");
    }

    try {
        if (compel_stop_task(pid_) < 0)
            throw CheckpointError("restore: compel_stop_task failed");

        // The privileged compel_prepare() sets up a netlink socket in the
        // target's netns and fails EPERM unprivileged; noctx skips that and
        // leaves the infect_ctx to us.
        ctl_ = compel_prepare_noctx_compat(pid_);
        if (!ctl_) throw CheckpointError("restore: compel_prepare_noctx failed");

        infect_ctx* ictx = compel_infect_ctx(ctl_);
        ictx->open_proc = target_open_proc;
        ictx->task_size = compel_task_size();
        ictx->log_fd = -1;

        // Bootstrap syscall_ip from any executable mapping the stub still has;
        // it moves into the reserve as soon as the reserve exists.
        std::uint64_t boot_ip = 0;
        for (const auto& m : checkpoint::read_maps(pid_))
            if (m.x) {
                boot_ip = m.start;
                break;
            }
        if (boot_ip == 0) throw CheckpointError("restore: stub has no executable mapping");
        ictx->syscall_ip = boot_ip;

        const long mapped = syscall(__NR_mmap, reserve_addr_, reserve_bytes_,
                                    PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, static_cast<std::uint64_t>(-1), 0);
        if (static_cast<std::uint64_t>(mapped) != reserve_addr_)
            throw CheckpointError("restore: could not reserve scratch at " + hex(reserve_addr_) +
                                  " (mmap returned " + hex(static_cast<std::uint64_t>(mapped)) + ")");

        // Everything from here runs out of the reserve, which survives eviction.
        ictx->syscall_ip = reserve_addr_;
        compel_set_leader_ip(ctl_, reserve_addr_);

        // Record the TLS block. compel restores saved registers around every
        // injected syscall, so fs_base cannot be moved and this VMA must be
        // spared during eviction.
        user_regs_struct regs{};
        ::iovec iov{&regs, sizeof regs};
        if (::ptrace(PTRACE_GETREGSET, pid_, reinterpret_cast<void*>(NT_PRSTATUS), &iov) != 0)
            throw CheckpointError(std::string("restore: GETREGSET on stub: ") + std::strerror(errno));
        for (const auto& m : checkpoint::read_maps(pid_))
            if (regs.fs_base >= m.start && regs.fs_base < m.end) {
                tls_start_ = m.start;
                tls_end_ = m.end;
                break;
            }
        if (tls_start_ == 0) throw CheckpointError("restore: could not locate the stub's TLS block");
    } catch (...) {
        kill_and_reap();
        throw;
    }
}

long RestoreTarget::syscall(int nr, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                            std::uint64_t a4, std::uint64_t a5, std::uint64_t a6) {
    long ret = 0;
    if (compel_syscall(ctl_, nr, &ret, a1, a2, a3, a4, a5, a6) < 0)
        throw CheckpointError("restore: could not inject syscall " + std::to_string(nr) +
                              " into pid " + std::to_string(pid_));
    return ret;
}

void RestoreTarget::kill_and_reap() {
    if (pid_ <= 0) return;
    ::kill(pid_, SIGKILL);
    int st = 0;
    while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {
    }
    pid_ = -1;
}

RestoreTarget::~RestoreTarget() {
    if (!released_) kill_and_reap();
}

}  // namespace adaptive_restore::restore
