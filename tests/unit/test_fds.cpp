#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "adaptive_restore/checkpoint/fds.hpp"
#include "adaptive_restore/common/error.hpp"

using namespace adaptive_restore;

// The child sets up a fd table we fully control, then stops so the parent can
// inspect it. Driving this cross-process matters: collect_fds() is always
// called on a stopped *target*, and a test that inspected itself would trip
// over the pipes ctest puts on stdout/stderr.
static constexpr int kChildSeek = 100;

int main() {
    char tmpl[] = "/tmp/arfdtestXXXXXX";
    const int seed_fd = ::mkstemp(tmpl);
    assert(seed_fd >= 0);
    assert(::ftruncate(seed_fd, 4096) == 0);
    ::close(seed_fd);

    struct stat want {};
    assert(::stat(tmpl, &want) == 0);

    const pid_t child = ::fork();
    assert(child >= 0);

    if (child == 0) {
        ::ptrace(PTRACE_TRACEME, 0, 0, 0);
        const int keep = ::open(tmpl, O_RDWR);
        if (keep < 0) _exit(11);
        if (::lseek(keep, kChildSeek, SEEK_SET) != kChildSeek) _exit(12);

        // Drop every inherited descriptor except the one under test, then point
        // 0/1/2 at /dev/null so the table holds only supported entries.
        for (int fd = 3; fd < 256; ++fd)
            if (fd != keep) ::close(fd);
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull < 0) _exit(13);
        if (::dup2(devnull, 0) < 0 || ::dup2(devnull, 1) < 0 || ::dup2(devnull, 2) < 0) _exit(14);
        if (devnull > 2) ::close(devnull);

        ::raise(SIGSTOP);
        _exit(0);
    }

    int st = 0;
    assert(::waitpid(child, &st, 0) == child);
    assert(WIFSTOPPED(st) && "child failed to reach its stop point");

    const auto fds = checkpoint::collect_fds(child);

    bool found = false;
    for (const auto& f : fds) {
        if (f.path != tmpl) continue;
        found = true;
        assert(f.pos == kChildSeek);
        assert(f.ino == want.st_ino);
        assert(f.dev == want.st_dev);
        assert((f.flags & O_ACCMODE) == O_RDWR);  // flags are octal in fdinfo
    }
    assert(found && "the child's regular file must be collected");

    // /dev/null is explicitly supported, so 0/1/2 all survive collection.
    int devnull_count = 0;
    for (const auto& f : fds)
        if (f.path == "/dev/null") ++devnull_count;
    assert(devnull_count == 3);
    assert(fds.size() == 4);

    ::ptrace(PTRACE_CONT, child, 0, 0);
    assert(::waitpid(child, &st, 0) == child);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    // An open pipe must make collection fail loudly rather than be skipped.
    int pp[2];
    assert(::pipe(pp) == 0);
    bool threw = false;
    try {
        checkpoint::collect_fds(::getpid());
    } catch (const CheckpointError&) {
        threw = true;
    }
    assert(threw && "open pipe fd must be rejected");
    ::close(pp[0]);
    ::close(pp[1]);

    ::unlink(tmpl);
    std::puts("test_fds: ok");
    return 0;
}
