#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <csignal>
#include <cstdio>

#include "adaptive_restore/checkpoint/maps.hpp"
#include "adaptive_restore/checkpoint/registers.hpp"
#include "adaptive_restore/common/error.hpp"

using namespace adaptive_restore;

int main() {
    const pid_t c = ::fork();
    assert(c >= 0);
    if (c == 0) {
        ::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        ::raise(SIGSTOP);
        _exit(0);
    }

    int st = 0;
    assert(::waitpid(c, &st, 0) == c);
    assert(WIFSTOPPED(st));

    const auto rr = checkpoint::capture_registers(c);
    assert(rr.record_version == 1);
    assert(rr.rip != 0 && rr.rsp != 0);
    assert(rr.xstate.size() >= 512);  // at least the legacy FXSAVE area

    // rip must land inside an executable mapping of the target, and rsp inside
    // its stack. This is what catches a mis-mapped user_regs_struct field.
    const auto maps = checkpoint::read_maps(c);
    bool rip_ok = false, rsp_ok = false;
    for (const auto& m : maps) {
        if (rr.rip >= m.start && rr.rip < m.end && m.x) rip_ok = true;
        if (rr.rsp >= m.start && rr.rsp < m.end && m.w) rsp_ok = true;
    }
    assert(rip_ok && "rip must fall in an executable mapping");
    assert(rsp_ok && "rsp must fall in a writable mapping");

    // cs/ss are the 64-bit user segments on x86-64.
    assert(rr.cs == 0x33 && rr.ss == 0x2b);
    // fs_base is where glibc keeps TLS, so it is set for any libc process.
    assert(rr.fs_base != 0);
    // Slot 15 is reserved: rsp travels in its own field.
    assert(rr.gpr[15] == 0);

    ::ptrace(PTRACE_CONT, c, nullptr, nullptr);
    assert(::waitpid(c, &st, 0) == c);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    // A pid that is not ours to trace must fail loudly.
    bool threw = false;
    try {
        checkpoint::capture_registers(c);  // reaped: no longer exists
    } catch (const CheckpointError&) {
        threw = true;
    }
    assert(threw && "capturing a dead pid must throw CheckpointError");

    std::puts("test_registers: ok");
    return 0;
}
