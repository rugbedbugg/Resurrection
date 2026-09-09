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

#include "adaptive_restore/checkpoint/proc_mem.hpp"
#include "adaptive_restore/common/error.hpp"

using namespace adaptive_restore::checkpoint;

// Child layout: page 0 carries a byte pattern, page 1 is all zero, page 2 is
// zero-filled fresh anonymous memory that is never touched.
static constexpr int kPages = 3;

int main() {
    const long pg = ::sysconf(_SC_PAGESIZE);
    assert(pg > 0);
    const std::size_t page = static_cast<std::size_t>(pg);

    int tube[2];
    assert(::pipe(tube) == 0);

    const pid_t child = ::fork();
    assert(child >= 0);

    if (child == 0) {
        ::close(tube[0]);
        ::ptrace(PTRACE_TRACEME, 0, 0, 0);
        auto* p = static_cast<unsigned char*>(::mmap(nullptr, page * kPages,
                                                     PROT_READ | PROT_WRITE,
                                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (p == MAP_FAILED) _exit(3);
        for (std::size_t i = 0; i < page; ++i) p[i] = static_cast<unsigned char>(i & 0xff);
        std::memset(p + page, 0, page);
        const std::uint64_t addr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
        if (::write(tube[1], &addr, sizeof addr) != static_cast<ssize_t>(sizeof addr)) _exit(4);
        ::close(tube[1]);
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

    ProcMem m(child);
    std::vector<std::byte> buf;

    // Page 0: the written pattern comes back byte for byte.
    m.read_range(base, page, buf);
    assert(buf.size() == page);
    assert(buf[7] == std::byte{7});
    for (std::size_t i = 0; i < page; ++i)
        assert(buf[i] == static_cast<std::byte>(i & 0xff));
    assert(!page_is_zero(buf));

    // Page 1: explicitly zeroed.
    m.read_range(base + page, page, buf);
    assert(page_is_zero(buf));

    // Page 2: never touched, so it is still the shared zero page. Reading it
    // must succeed rather than fault, since capture reads whole regions.
    m.read_range(base + 2 * page, page, buf);
    assert(page_is_zero(buf));

    // A multi-page read in one call spans all three pages, and the buffer is
    // resized to exactly the requested length.
    m.read_range(base, page * kPages, buf);
    assert(buf.size() == page * kPages);
    assert(buf[7] == std::byte{7});
    assert(page_is_zero({buf.data() + page, page}));

    // A zero-length read is legal and yields an empty buffer.
    m.read_range(base, 0, buf);
    assert(buf.empty());

    // page_is_zero on an empty span is vacuously true.
    assert(page_is_zero({}));

    // An unmapped address must throw rather than return garbage.
    bool threw = false;
    try {
        m.read_range(0x10, page, buf);
    } catch (const adaptive_restore::CheckpointError&) {
        threw = true;
    }
    assert(threw && "reading an unmapped address must throw CheckpointError");

    ::ptrace(PTRACE_CONT, child, 0, 0);
    assert(::waitpid(child, &st, 0) == child);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    std::puts("test_proc_mem: ok");
    return 0;
}
