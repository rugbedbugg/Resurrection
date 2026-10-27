// Failure tests: userfaultfd unavailable.
//
// Two ways a deferring restore can be denied its userfaultfd, and in both it
// must fail explicitly, before control transfer, with no process left behind.
//
// 1. The target's userfaultfd() call itself fails. Simulated by dropping the
//    stub's RLIMIT_NOFILE to zero, so the kernel has no descriptor to give.
//    The errno differs from a kernel without userfaultfd (ENOSYS), but the
//    failure path is the same: the call happens inside the target and every
//    negative result is handled alike.
// 2. A seccomp filter denies userfaultfd, as Docker's default profile does. The
//    stub inherits it, and compel cannot seize a filtered task without
//    CAP_SYS_ADMIN, so restore fails earlier, at seizing. That is still an
//    explicit failure. The filter cannot be removed once installed, so this
//    case runs last.
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/restore/restore.hpp"
#include "adaptive_restore/restore/uffd.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::restore;

static constexpr std::uint64_t kReserve = 0x700000000000ULL;
static constexpr std::size_t kReserveBytes = 4 * 4096;
static constexpr std::uint64_t kAnon = 0x100000000000ULL;

static void assert_no_stray_children(const char* what) {
    int st = 0;
    const pid_t p = ::waitpid(-1, &st, WNOHANG);
    if (p > 0) {
        std::fprintf(stderr, "%s: stray child %d survived\n", what, p);
        assert(false && "a failed restore must not leave a process behind");
    }
    assert(p < 0 && errno == ECHILD);
}

static void deny_userfaultfd() {
    sock_filter prog[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_userfaultfd, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog fprog{static_cast<unsigned short>(sizeof prog / sizeof prog[0]), prog};
    assert(::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
    assert(::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog) == 0);
}

int main() {
    // --- the target's userfaultfd() fails -----------------------------------
    {
        std::string what;
        try {
            RestoreTarget target(kReserve, kReserveBytes);
            const rlimit none{0, 0};
            assert(::prlimit(target.pid(), RLIMIT_NOFILE, &none, nullptr) == 0);
            Userfault uffd(target);
        } catch (const std::exception& e) {
            what = e.what();
        }
        std::fprintf(stderr, "userfaultfd failure: %s\n", what.c_str());
        assert(what.find("userfaultfd") != std::string::npos &&
               "a failed userfaultfd() must be reported as such, not ignored");
        assert_no_stray_children("userfaultfd failure");
    }

    // --- a seccomp filter denies userfaultfd --------------------------------
    {
        ImageWriter w;
        ImageHeaderInfo h{};
        h.page_size = 4096;
        h.kernel_release = "test";
        w.set_header(h);
        w.set_registers(RegisterRecord{});
        w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                       kAnon, kAnon + 4096, 0, 0, 0, ""});
        const std::vector<std::byte> page(4096, std::byte{0x33});
        w.add_page({kAnon, 0, 0, PageEncoding::Raw, 0}, page);
        w.add_region({0, 0, kAnon, kAnon + 4096, 0, 1});
        const auto bytes = w.serialize_to_bytes();

        deny_userfaultfd();
        // The premise, checked rather than assumed.
        assert(::syscall(__NR_userfaultfd, 0) == -1 && errno == EPERM);

        bool threw = false;
        try {
            auto img = ImageReader::from_bytes(bytes);
            RestoreOptions o;
            o.policy = policy::RestorePolicy::Lazy;
            restore_image(img, o);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "seccomp-filtered restore: %s\n", e.what());
            threw = true;
        }
        assert(threw && "restore under a filter that denies userfaultfd must fail");
        assert_no_stray_children("seccomp-filtered restore");
    }

    std::puts("test_uffd_unavailable: ok");
    return 0;
}
