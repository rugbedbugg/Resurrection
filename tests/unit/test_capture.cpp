#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "adaptive_restore/checkpoint/capture.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::checkpoint;

int main() {
    const long pg = ::sysconf(_SC_PAGESIZE);
    assert(pg == 4096);
    const std::size_t page = static_cast<std::size_t>(pg);

    int tube[2];
    assert(::pipe(tube) == 0);

    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        ::close(tube[0]);
        ::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        auto* p = static_cast<unsigned char*>(::mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (p == MAP_FAILED) _exit(3);
        for (std::size_t i = 0; i < page; ++i) p[i] = static_cast<unsigned char>(i & 0xff);
        std::memset(p + page, 0, page);  // second page stays all zero
        const std::uint64_t a = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
        if (::write(tube[1], &a, sizeof a) != static_cast<ssize_t>(sizeof a)) _exit(4);
        ::close(tube[1]);

        // This child is forked directly rather than through spawn_to_marker, so
        // it has to sanitise its own fd table: the harness may hand us a socket
        // or pipe on 0/1/2, which Tier 1 cannot represent.
        for (int fd = 3; fd < 256; ++fd) ::close(fd);
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull < 0) _exit(5);
        if (::dup2(devnull, 0) < 0 || ::dup2(devnull, 1) < 0 || ::dup2(devnull, 2) < 0) _exit(6);
        if (devnull > 2) ::close(devnull);

        ::raise(SIGSTOP);
        _exit(0);
    }

    ::close(tube[1]);
    std::uint64_t base = 0;
    assert(::read(tube[0], &base, sizeof base) == static_cast<ssize_t>(sizeof base));
    ::close(tube[0]);

    int st = 0;
    assert(::waitpid(child, &st, 0) == child);
    assert(WIFSTOPPED(st));

    ImageWriter w;
    CaptureOptions o;
    o.verify = true;
    const auto res = capture(child, std::array<std::byte, 32>{}, o, w);

    assert(res.verify_mismatches == 0);
    assert(res.pages_saved >= 2 && res.zero_pages >= 1);
    assert(res.mappings_total > res.mappings_saved);  // vdso/vvar recorded, not saved
    assert(res.regions_created > 0);
    assert(res.fds_saved > 0);
    assert(res.bytes_saved == (res.pages_saved - res.zero_pages) * 4096);

    // The image round-trips and validates.
    auto bytes = w.serialize_to_bytes();
    auto ir = ImageReader::from_bytes(bytes);
    ir.verify();
    assert(ir.header().original_pid == static_cast<std::uint32_t>(child));
    assert(!ir.header().kernel_release.empty());
    assert(ir.registers().rip != 0);

    const auto pidx = ir.page_index();
    assert(pidx.size() == res.pages_saved);

    // Both of the child's known pages are present, with the right contents.
    bool pattern_ok = false, zero_ok = false;
    for (const auto& p : pidx) {
        if (p.vaddr == base) {
            assert(p.encoding == PageEncoding::Raw);
            auto pb = ir.page_bytes(p);
            assert(pb.size() == page);
            bool all = true;
            for (std::size_t i = 0; i < page; ++i)
                if (pb[i] != static_cast<std::byte>(i & 0xff)) all = false;
            pattern_ok = all;
        }
        if (p.vaddr == base + page) {
            assert(p.encoding == PageEncoding::Zero);
            zero_ok = ir.page_bytes(p).size() == page;
        }
    }
    assert(pattern_ok && "the child's pattern page must survive capture byte for byte");
    assert(zero_ok && "the child's zero page must be elided as PageEncoding::Zero");

    // Kernel-special mappings are recorded for layout but carry no contents.
    bool saw_kernel_special = false;
    for (const auto& m : ir.mappings())
        if (m.flags & MappingFlag::KernelSpecial) {
            saw_kernel_special = true;
            assert((m.flags & MappingFlag::ContentsSaved) == 0);
        }
    assert(saw_kernel_special);

    // Regions tile their mapping and index a contiguous page range.
    const auto regs = ir.regions();
    assert(regs.size() == res.regions_created);
    for (const auto& r : regs) assert(r.page_index_hi > r.page_index_lo);

    ::ptrace(PTRACE_CONT, child, nullptr, nullptr);
    assert(::waitpid(child, &st, 0) == child);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    std::puts("test_capture: ok");
    return 0;
}
