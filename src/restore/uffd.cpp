#include "adaptive_restore/restore/uffd.hpp"

#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"

namespace adaptive_restore::restore {

namespace {

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}

bool overlaps(std::uint64_t a0, std::uint64_t a1, std::uint64_t b0, std::uint64_t b1) {
    return a0 < b1 && b0 < a1;
}

}  // namespace

Userfault::Userfault(RestoreTarget& target)
    : reserve_start_(target.reserve_addr()),
      reserve_end_(target.reserve_addr() + target.reserve_bytes()),
      tls_start_(target.tls_start()),
      tls_end_(target.tls_end()) {
    // UFFD_USER_MODE_ONLY is the Phase 0 decision: it works even where
    // vm.unprivileged_userfaultfd is 0, and there is no fallback.
    // O_NONBLOCK matters more than it looks: the kernel's userfaultfd_poll()
    // returns EPOLLERR outright for a blocking fd, because poll() cannot
    // promise that a subsequent read() will not block. Without it every poll
    // reports an error, no fault is ever read, and the target sits in
    // handle_userfault forever.
    const long remote =
        target.syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (remote < 0)
        throw CheckpointError(std::string("restore: target userfaultfd(): ") +
                              std::strerror(static_cast<int>(-remote)));

    const long pidfd = ::syscall(SYS_pidfd_open, target.pid(), 0);
    if (pidfd < 0)
        throw CheckpointError(std::string("restore: pidfd_open: ") + std::strerror(errno));
    const long stolen = ::syscall(SYS_pidfd_getfd, static_cast<int>(pidfd),
                                  static_cast<int>(remote), 0);
    const int saved = errno;
    ::close(static_cast<int>(pidfd));
    if (stolen < 0)
        throw CheckpointError(std::string("restore: pidfd_getfd: ") + std::strerror(saved));
    fd_ = static_cast<int>(stolen);
    target_fd_ = static_cast<int>(remote);

    // The target keeps its own reference on purpose. If this page server dies,
    // that reference keeps the uffd alive so faults stall rather than silently
    // resolving to zero pages, and the death signal below turns the stall into
    // a clean kill. (docs/phase3-spike-report.md, Q3.)
    ::uffdio_api api{};
    api.api = UFFD_API;
    if (::ioctl(fd_, UFFDIO_API, &api) != 0) {
        ::close(fd_);
        fd_ = -1;
        throw CheckpointError(std::string("restore: UFFDIO_API: ") + std::strerror(errno));
    }
    features_ = api.features;

    // Without this, losing the page server leaves the target wedged in
    // handle_userfault forever. Must be armed before control transfer, because
    // afterwards no more syscalls can be injected.
    const long rc = target.syscall(__NR_prctl, PR_SET_PDEATHSIG, SIGKILL);
    if (rc != 0) {
        ::close(fd_);
        fd_ = -1;
        throw CheckpointError(std::string("restore: could not arm the target's death signal: ") +
                              std::strerror(static_cast<int>(-rc)));
    }
}

Userfault::~Userfault() {
    if (fd_ >= 0) ::close(fd_);
}

void Userfault::register_missing(std::uint64_t start, std::uint64_t end) {
    if (end <= start) throw CheckpointError("restore: empty userfaultfd range");
    if (overlaps(start, end, reserve_start_, reserve_end_))
        throw CheckpointError("restore: refusing to register the compel reserve at " +
                              hex(reserve_start_));
    if (overlaps(start, end, tls_start_, tls_end_))
        throw CheckpointError("restore: refusing to register the stub's TLS at " +
                              hex(tls_start_));

    ::uffdio_register reg{};
    reg.range.start = start;
    reg.range.len = end - start;
    reg.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (::ioctl(fd_, UFFDIO_REGISTER, &reg) != 0)
        throw CheckpointError("restore: UFFDIO_REGISTER " + hex(start) + "-" + hex(end) + ": " +
                              std::strerror(errno));
    registered_bytes_ += end - start;
}

}  // namespace adaptive_restore::restore
