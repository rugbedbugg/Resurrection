#include "adaptive_restore/checkpoint/registers.hpp"

#include <cpuid.h>
#include <elf.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "adaptive_restore/common/error.hpp"

namespace adaptive_restore::checkpoint {

namespace {

// Comfortably covers x87/SSE/AVX (832 bytes on a typical machine); the growth
// loop below handles AVX-512 and AMX, whose areas run to ~11 KiB.
constexpr std::size_t kXstateFirstTry = 4096;
constexpr std::size_t kXstateMaxBytes = 64 * 1024;

// XCR0 is uniform for user processes on a machine, so reading our own is a
// faithful record of the mask the target ran under. The blob's own XSTATE_BV
// (bytes 512..519, which components are actually present rather than enabled)
// travels inside xstate for a restore to consult.
std::uint64_t read_xcr0() {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    if (((c >> 27) & 1u) == 0) return 0;  // OSXSAVE clear: XCR0 not accessible
    std::uint32_t lo = 0, hi = 0;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

bool getregset(pid_t pid, int nt, void* buf, std::size_t& len) {
    ::iovec iov{buf, len};
    if (::ptrace(PTRACE_GETREGSET, pid, reinterpret_cast<void*>(static_cast<std::intptr_t>(nt)),
                 &iov) != 0)
        return false;
    len = iov.iov_len;
    return true;
}

// PTRACE_GETREGSET clamps iov_len to the buffer it was given rather than
// reporting the size it wanted, so a result that exactly fills the buffer is
// indistinguishable from a truncated one. Grow and retry until it comes back
// short, which is the only proof the whole area was copied.
std::vector<std::byte> read_xstate(pid_t pid) {
    for (std::size_t cap = kXstateFirstTry; cap <= kXstateMaxBytes; cap *= 2) {
        std::vector<std::byte> buf(cap);
        std::size_t len = cap;
        if (!getregset(pid, NT_X86_XSTATE, buf.data(), len)) break;
        if (len < cap) {
            buf.resize(len);
            return buf;
        }
    }

    // No xstate regset (or it never came back short): fall back to the legacy
    // 512-byte FXSAVE area, which every x86-64 kernel provides.
    std::vector<std::byte> fp(sizeof(user_fpregs_struct));
    std::size_t len = fp.size();
    if (!getregset(pid, NT_PRFPREG, fp.data(), len))
        throw CheckpointError("registers: PTRACE_GETREGSET(NT_PRFPREG) on pid " +
                              std::to_string(pid) + ": " + std::strerror(errno));
    fp.resize(len);
    return fp;
}

}  // namespace

image::RegisterRecord capture_registers(pid_t pid) {
    user_regs_struct regs{};
    std::size_t len = sizeof regs;
    if (!getregset(pid, NT_PRSTATUS, &regs, len))
        throw CheckpointError("registers: PTRACE_GETREGSET(NT_PRSTATUS) on pid " +
                              std::to_string(pid) + ": " + std::strerror(errno));
    if (len != sizeof regs)
        throw CheckpointError("registers: NT_PRSTATUS returned " + std::to_string(len) +
                              " bytes, expected " + std::to_string(sizeof regs));

    image::RegisterRecord rr;
    rr.rip = regs.rip;
    rr.rsp = regs.rsp;
    rr.rflags = regs.eflags;

    // gpr[0..14] in the order frozen by image/format.hpp. rsp is carried in its
    // own field, so slot 15 stays zero and is reserved.
    rr.gpr[0] = regs.rax;
    rr.gpr[1] = regs.rbx;
    rr.gpr[2] = regs.rcx;
    rr.gpr[3] = regs.rdx;
    rr.gpr[4] = regs.rsi;
    rr.gpr[5] = regs.rdi;
    rr.gpr[6] = regs.rbp;
    rr.gpr[7] = regs.r8;
    rr.gpr[8] = regs.r9;
    rr.gpr[9] = regs.r10;
    rr.gpr[10] = regs.r11;
    rr.gpr[11] = regs.r12;
    rr.gpr[12] = regs.r13;
    rr.gpr[13] = regs.r14;
    rr.gpr[14] = regs.r15;

    rr.fs_base = regs.fs_base;
    rr.gs_base = regs.gs_base;
    rr.cs = static_cast<std::uint16_t>(regs.cs);
    rr.ss = static_cast<std::uint16_t>(regs.ss);
    rr.ds = static_cast<std::uint16_t>(regs.ds);
    rr.es = static_cast<std::uint16_t>(regs.es);
    rr.fs = static_cast<std::uint16_t>(regs.fs);
    rr.gs = static_cast<std::uint16_t>(regs.gs);
    rr.orig_rax = regs.orig_rax;

    rr.xcr0 = read_xcr0();
    rr.xstate = read_xstate(pid);
    return rr;
}

}  // namespace adaptive_restore::checkpoint
