#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/restore/address_space.hpp"
#include "adaptive_restore/restore/uffd.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::restore;

static constexpr std::uint64_t kReserve = 0x700000000000ULL;
static constexpr std::size_t kReserveBytes = 4 * 4096;
static constexpr std::uint64_t kAnon = 0x100000000000ULL;
static constexpr std::uint64_t kFile = 0x100000010000ULL;
static constexpr std::uint64_t kShared = 0x100000020000ULL;

int main() {
    // --- which mappings are eligible for MISSING mode -----------------------
    {
        ImageWriter w;
        ImageHeaderInfo h{};
        h.page_size = 4096;
        h.kernel_release = "test";
        w.set_header(h);
        w.set_registers(RegisterRecord{});
        w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                       kAnon, kAnon + 4096, 0, 0, 0, ""});
        w.add_mapping({0, MappingFlag::Read | MappingFlag::FileBacked | MappingFlag::ContentsSaved,
                       kFile, kFile + 4096, 0, 1, 2, "/usr/lib/libc.so.6"});
        w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::Shared |
                              MappingFlag::ContentsSaved,
                       kShared, kShared + 4096, 0, 0, 0, ""});
        w.add_mapping({0, MappingFlag::Read | MappingFlag::KernelSpecial,
                       0x7ffff7fb6000ULL, 0x7ffff7fba000ULL, 0, 0, 0, "[vdso]"});
        w.add_page({kAnon, 0, 0, PageEncoding::Zero, 0}, {});
        w.add_region({0, 0, kAnon, kAnon + 4096, 0, 1});
        auto bytes = w.serialize_to_bytes();
        auto image = ImageReader::from_bytes(bytes);

        const auto ranges = lazy_ranges(image);
        assert(ranges.size() == 1 && "only private anonymous saved mappings are eligible");
        assert(ranges[0].start == kAnon && ranges[0].end == kAnon + 4096);
    }

    // --- create, steal, register, and actually serve a fault ----------------
    {
        RestoreTarget target(kReserve, kReserveBytes);

        Userfault uffd(target);
        assert(uffd.fd() >= 0);
        assert(uffd.features() != 0);

        // A scratch anonymous region in the target, registered MISSING.
        const long addr = target.syscall(__NR_mmap, 0, 2 * 4096, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS,
                                         static_cast<std::uint64_t>(-1), 0);
        assert(addr > 0);
        const auto base = static_cast<std::uint64_t>(addr);
        uffd.register_missing(base, base + 2 * 4096);
        assert(uffd.registered_bytes() == 2 * 4096);

        // compel must keep working after registration; Task 4 builds the
        // server that actually resolves faults in this range.
        assert(target.syscall(__NR_getpid) == target.pid());

        // The reserve and the TLS must be refused outright.
        bool threw = false;
        try {
            uffd.register_missing(kReserve, kReserve + 4096);
        } catch (const CheckpointError& e) {
            threw = std::string(e.what()).find("compel reserve") != std::string::npos;
        }
        assert(threw && "registering the reserve must be refused");

        threw = false;
        try {
            uffd.register_missing(target.tls_start(), target.tls_end());
        } catch (const CheckpointError& e) {
            threw = std::string(e.what()).find("TLS") != std::string::npos;
        }
        assert(threw && "registering the stub's TLS must be refused");
    }

    // --- the safety mechanism: a dead page server must not leave a wedged
    // --- target behind (docs/phase3-spike-report.md, Q3) --------------------
    {
        int tube[2];
        assert(::pipe(tube) == 0);
        const pid_t helper = ::fork();
        assert(helper >= 0);
        if (helper == 0) {
            // Stand in for arctl: build a target, arm its death signal, then
            // die without cleaning up, exactly as a crashed page server would.
            ::close(tube[0]);
            auto target = new RestoreTarget(kReserve, kReserveBytes);
            new Userfault(*target);  // deliberately leaked; this process is about to die
            const pid_t stub = target->pid();
            target->release();       // do not let the destructor kill it
            if (::write(tube[1], &stub, sizeof stub) != sizeof stub) _exit(2);
            _exit(0);
        }
        ::close(tube[1]);
        pid_t stub = -1;
        assert(::read(tube[0], &stub, sizeof stub) == sizeof stub);
        ::close(tube[0]);
        int st = 0;
        assert(::waitpid(helper, &st, 0) == helper);

        // PR_SET_PDEATHSIG fires when the parent dies, so the stub must be gone.
        bool alive = true;
        for (int i = 0; i < 200 && alive; ++i) {
            if (::kill(stub, 0) != 0) alive = false;
            else ::usleep(10000);
        }
        assert(!alive && "a target whose page server died must be killed, not left wedged");
    }

    std::puts("test_uffd: ok");
    return 0;
}
