#include "adaptive_restore/paging/lazy_provider.hpp"

#include <linux/userfaultfd.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <time.h>
#include <string>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"

namespace adaptive_restore::paging {

namespace {

std::string hex(std::uint64_t v) {
    char b[19];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return std::string(b);
}

}  // namespace

LazyProvider::LazyProvider(const image::ImageReader& image, restore::Userfault& uffd)
    : image_(image), uffd_(uffd), locator_(image), regions_(image.regions()) {}

void LazyProvider::prepare_region(std::uint32_t region_id, RestoreAction action) {
    if (action != RestoreAction::Lazy)
        throw CheckpointError("LazyProvider: asked to prepare region " +
                              std::to_string(region_id) + " with a non-lazy action");

    // Cached in the constructor: parsing the region table per region is
    // quadratic and was worth 200 ms on a 64 MiB image.
    if (region_id >= regions_.size())
        throw CheckpointError("LazyProvider: no such region " + std::to_string(region_id));
    const auto& r = regions_[region_id];

    // Nothing is written; registering the range is the whole preparation.
    uffd_.register_missing(r.start_vaddr, r.end_vaddr);
    ++stats_.regions_registered;
    stats_.pages_registered += r.page_index_hi - r.page_index_lo;
}

namespace {
std::uint64_t monotonic_ns() {
    ::timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}
}  // namespace

void LazyProvider::write_trace(const std::string& path) const {
    std::ofstream f(path);
    if (!f) throw CheckpointError("restore: cannot write the fault trace to " + path);
    f << "vaddr\tregion\toffset_ns\tservice_ns\tbytes\tencoding\n";
    for (const auto& r : trace_)
        f << "0x" << std::hex << r.vaddr << std::dec << '\t' << r.region_id << '\t'
          << r.offset_ns << '\t' << r.service_ns << '\t' << r.bytes << '\t'
          << static_cast<unsigned>(r.encoding) << '\n';
    if (!f) throw CheckpointError("restore: failed while writing the fault trace to " + path);
}

void LazyProvider::service_fault(const PageRequest& request) {
    const std::uint64_t began = monotonic_ns();
    if (serving_started_ns_ == 0) serving_started_ns_ = began;

    const std::size_t idx = locator_.find(request.virtual_address);
    if (idx == PageLocator::npos)
        throw CheckpointError("restore: fault at " + hex(request.virtual_address) +
                              " has no page in the image");

    const auto& p = locator_.pages()[idx];
    const std::uint64_t page_size = locator_.page_size();
    const std::uint64_t aligned = p.vaddr;

    switch (p.encoding) {
    case image::PageEncoding::Zero: {
        ::uffdio_zeropage z{};
        z.range.start = aligned;
        z.range.len = page_size;
        if (::ioctl(uffd_.fd(), UFFDIO_ZEROPAGE, &z) != 0)
            throw CheckpointError("restore: UFFDIO_ZEROPAGE at " + hex(aligned) + ": " +
                                  std::strerror(errno));
        ++stats_.zero_faults;
        break;
    }
    case image::PageEncoding::Raw: {
        const auto bytes = image_.page_bytes(p);  // re-checks the page CRC
        ::uffdio_copy c{};
        c.dst = aligned;
        c.src = reinterpret_cast<std::uint64_t>(bytes.data());
        c.len = page_size;
        if (::ioctl(uffd_.fd(), UFFDIO_COPY, &c) != 0)
            throw CheckpointError("restore: UFFDIO_COPY at " + hex(aligned) + ": " +
                                  std::strerror(errno));
        ++stats_.copy_faults;
        stats_.bytes_supplied += page_size;
        break;
    }
    case image::PageEncoding::FromFile:
        // Only private anonymous ranges are registered, and a referenced page
        // only exists inside a file-backed mapping, so this cannot happen
        // unless range selection has gone wrong.
        throw CheckpointError("restore: a file-referenced page at " + hex(aligned) +
                              " was registered for demand paging; range selection is wrong");
    case image::PageEncoding::Omitted:
        throw CheckpointError("restore: page at " + hex(aligned) +
                              " was omitted at capture and cannot be supplied");
    }
    ++stats_.faults_served;

    const std::uint64_t ended = monotonic_ns();
    stats_.service_ns_total += ended - began;
    if (recording_)
        trace_.push_back({p.vaddr, request.region_id, began - serving_started_ns_,
                          ended - began,
                          static_cast<std::uint32_t>(
                              p.encoding == image::PageEncoding::Raw ? page_size : 0),
                          static_cast<std::uint8_t>(p.encoding)});
}

ServeResult serve_faults(restore::Userfault& uffd, PageProvider& provider,
                         const PageLocator& locator, pid_t target_pid) {
    ServeResult out;

    // Poll the target's pidfd alongside the uffd so its exit is noticed the
    // moment it happens. Waiting on a poll timeout instead adds that timeout to
    // every run, which is pure measurement artifact: it dominated the numbers
    // before this was fixed.
    const long pidfd = ::syscall(SYS_pidfd_open, target_pid, 0);

    const auto reap = [target_pid, &out]() {
        int st = 0;
        const pid_t w = ::waitpid(target_pid, &st, WNOHANG);
        if (w == target_pid) {
            out.exit_status = st;
            return true;
        }
        return w < 0 && errno == ECHILD;
    };

    for (;;) {
        ::pollfd pfds[2];
        pfds[0] = {uffd.fd(), POLLIN, 0};
        pfds[1] = {static_cast<int>(pidfd), POLLIN, 0};
        const int nfds = pidfd >= 0 ? 2 : 1;

        const int pr = ::poll(pfds, static_cast<nfds_t>(nfds), 1000);
        if (pr < 0) {
            if (errno == EINTR) continue;
            ::kill(target_pid, SIGKILL);
            if (pidfd >= 0) ::close(static_cast<int>(pidfd));
            throw CheckpointError(std::string("restore: poll on the userfaultfd: ") +
                                  std::strerror(errno));
        }

        // Faults first: the target may have queued one and then exited, and
        // dropping it would be indistinguishable from never serving it.
        if (pr == 0 || !(pfds[0].revents & (POLLIN | POLLERR | POLLHUP))) {
            if ((nfds == 2 && (pfds[1].revents & POLLIN)) || reap()) {
                reap();
                out.target_exited = true;
                break;
            }
            continue;
        }

        if (pfds[0].revents & POLLHUP) {
            reap();
            out.target_exited = true;
            break;
        }
        if ((pfds[0].revents & POLLERR) && !(pfds[0].revents & POLLIN)) {
            // Not "the target went away": the kernel reports EPOLLERR for a
            // userfaultfd that is uninitialised or not O_NONBLOCK. Treating it
            // as a clean exit is how a mis-created uffd turns into a target
            // that silently reads zero pages.
            if (reap()) {
                out.target_exited = true;
                break;
            }
            ::kill(target_pid, SIGKILL);
            if (pidfd >= 0) ::close(static_cast<int>(pidfd));
            throw CheckpointError(
                "restore: userfaultfd poll reported POLLERR while the target is alive; "
                "the fd is uninitialised or was not created with O_NONBLOCK");
        }

        ::uffd_msg msg{};
        const ssize_t n = ::read(uffd.fd(), &msg, sizeof msg);
        if (n == 0) {
            reap();
            out.target_exited = true;
            break;
        }
        if (n != sizeof msg) {
            if (errno == EAGAIN || errno == EINTR) continue;
            ::kill(target_pid, SIGKILL);
            if (pidfd >= 0) ::close(static_cast<int>(pidfd));
            throw CheckpointError(std::string("restore: short read on the userfaultfd: ") +
                                  std::strerror(errno));
        }

        switch (msg.event) {
        case UFFD_EVENT_PAGEFAULT: {
            const std::uint64_t addr = msg.arg.pagefault.address & ~(locator.page_size() - 1);
            const std::size_t idx = locator.find(addr);
            PageRequest req;
            req.virtual_address = addr;
            if (idx != PageLocator::npos) {
                req.image_offset = locator.pages()[idx].store_offset;
                req.region_id = locator.region_of(idx);
            }
            try {
                provider.service_fault(req);
            } catch (...) {
                // A fault we cannot resolve leaves the target blocked forever,
                // so it must die rather than wedge.
                ::kill(target_pid, SIGKILL);
                if (pidfd >= 0) ::close(static_cast<int>(pidfd));
                throw;
            }
            ++out.faults_served;
            break;
        }
        case UFFD_EVENT_UNMAP:
        case UFFD_EVENT_REMOVE:
            break;
        default:
            break;
        }
    }

    if (pidfd >= 0) ::close(static_cast<int>(pidfd));
    return out;
}

}  // namespace adaptive_restore::paging
