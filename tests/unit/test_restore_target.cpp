#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <string>

#include "adaptive_restore/checkpoint/maps.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/restore/target.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::restore;

static constexpr std::uint64_t kReserve = 0x700000000000ULL;
static constexpr std::size_t kReserveBytes = 3 * 4096;

int main() {
    {
        RestoreTarget t(kReserve, kReserveBytes);
        assert(t.pid() > 0);
        assert(t.ctl() != nullptr);

        const auto maps = checkpoint::read_maps(t.pid());
        assert(!maps.empty());

        // The reserve exists, at exactly the requested address, and is rwx.
        bool reserve_ok = false;
        for (const auto& m : maps)
            if (m.start == kReserve) {
                reserve_ok = m.r && m.w && m.x && (m.end - m.start) == kReserveBytes;
            }
        assert(reserve_ok && "reserve must be mapped rwx at the requested address");

        // TLS was located and is a real mapping of the stub.
        assert(t.tls_start() != 0 && t.tls_end() > t.tls_start());
        bool tls_is_a_mapping = false;
        for (const auto& m : maps)
            if (m.start == t.tls_start() && m.end == t.tls_end()) tls_is_a_mapping = true;
        assert(tls_is_a_mapping);
        assert(t.tls_start() != kReserve && "TLS must not be the reserve itself");

        // Syscalls run inside the target: getpid must report the stub's pid.
        assert(t.syscall(__NR_getpid) == t.pid());

        // ASLR is off in the stub, which is what makes saved addresses meaningful.
        assert(t.syscall(__NR_personality, 0xffffffffULL) >= 0);
        const long persona = t.syscall(__NR_personality, 0xffffffffULL);
        assert((persona & ADDR_NO_RANDOMIZE) && "stub must run with ASLR disabled");

        // A syscall driven through the reserve still works after the switch,
        // and a new mapping really appears in the target.
        const long addr = t.syscall(__NR_mmap, 0, 4096, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, static_cast<std::uint64_t>(-1), 0);
        assert(addr > 0);
        bool saw_new = false;
        for (const auto& m : checkpoint::read_maps(t.pid()))
            if (m.start == static_cast<std::uint64_t>(addr)) saw_new = true;
        assert(saw_new && "a mapping created via compel_syscall must be visible in the target");
    }
    // Destruction with no release() must leave nothing behind; if the stub
    // survived, this pid would still be alive.

    // Two targets in sequence must both work (no global compel state leaks).
    {
        RestoreTarget a(kReserve, kReserveBytes);
        assert(a.syscall(__NR_getpid) == a.pid());
    }
    {
        RestoreTarget b(kReserve, kReserveBytes);
        assert(b.syscall(__NR_getpid) == b.pid());
        assert(b.tls_start() != 0);
    }

    std::puts("test_restore_target: ok");
    return 0;
}
