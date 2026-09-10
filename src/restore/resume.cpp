#include "adaptive_restore/restore/resume.hpp"

#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/restore/address_space.hpp"
#include "adaptive_restore/restore/compel_compat.hpp"

namespace adaptive_restore::restore {

namespace {

std::vector<int> live_fds(pid_t pid) {
    const std::string dir = "/proc/" + std::to_string(pid) + "/fd";
    DIR* d = ::opendir(dir.c_str());
    if (!d) throw CheckpointError("restore: cannot list " + dir);
    std::vector<int> out;
    while (const dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        char* end = nullptr;
        const long n = std::strtol(e->d_name, &end, 10);
        if (end != e->d_name && *end == '\0') out.push_back(static_cast<int>(n));
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

void stage_path(RestoreTarget& t, const std::string& path, std::uint64_t at) {
    ::iovec local{const_cast<char*>(path.c_str()), path.size() + 1};
    ::iovec remote{reinterpret_cast<void*>(static_cast<std::uintptr_t>(at)), path.size() + 1};
    if (::process_vm_writev(t.pid(), &local, 1, &remote, 1, 0) !=
        static_cast<ssize_t>(path.size() + 1))
        throw CheckpointError("restore: could not stage fd path: " + path);
}

}  // namespace

std::uint64_t restore_fds(RestoreTarget& target, const image::ImageReader& image) {
    const auto fds = image.fds();
    const std::uint64_t scratch = target.reserve_addr() + target.reserve_bytes() - 4096;

    std::uint64_t restored = 0;
    for (const auto& f : fds) {
        struct stat st {};
        if (::stat(f.path.c_str(), &st) != 0)
            throw CheckpointError("restore: fd " + std::to_string(f.fd) +
                                  ": backing file is gone: " + f.path);
        // /dev/null is a character device shared by everyone; identity is not
        // meaningful there, but a regular file that moved is a real problem.
        if (f.path != "/dev/null" &&
            (static_cast<std::uint64_t>(st.st_dev) != f.dev ||
             static_cast<std::uint64_t>(st.st_ino) != f.ino))
            throw CheckpointError("restore: fd " + std::to_string(f.fd) +
                                  ": backing file changed since checkpoint: " + f.path);

        stage_path(target, f.path, scratch);
        const long tmp = target.syscall(__NR_openat, static_cast<std::uint64_t>(AT_FDCWD), scratch,
                                        f.flags & ~static_cast<std::uint32_t>(O_CREAT | O_EXCL), 0);
        if (tmp < 0)
            throw CheckpointError("restore: target could not open " + f.path + ": " +
                                  std::strerror(static_cast<int>(-tmp)));

        if (static_cast<std::uint32_t>(tmp) != f.fd) {
            // dup2(x, x) would not close x, so the close below must be skipped
            // in that case -- it would shut the descriptor we just installed.
            const long dup = target.syscall(__NR_dup2, static_cast<std::uint64_t>(tmp), f.fd);
            if (dup < 0)
                throw CheckpointError("restore: dup2 onto fd " + std::to_string(f.fd) + ": " +
                                      std::strerror(static_cast<int>(-dup)));
            target.syscall(__NR_close, static_cast<std::uint64_t>(tmp));
        }

        if (f.pos != 0) {
            const long pos = target.syscall(__NR_lseek, f.fd, f.pos, SEEK_SET);
            if (pos < 0 || static_cast<std::uint64_t>(pos) != f.pos)
                throw CheckpointError("restore: could not seek fd " + std::to_string(f.fd) +
                                      " to " + std::to_string(f.pos));
        }
        ++restored;
    }

    // Anything the stub still holds that the image did not record must go, or
    // the restored process would own descriptors it never opened.
    std::uint64_t closed = 0;
    for (const int fd : live_fds(target.pid())) {
        const bool wanted = std::any_of(fds.begin(), fds.end(), [fd](const image::FdRecord& r) {
            return static_cast<int>(r.fd) == fd;
        });
        if (!wanted) {
            target.syscall(__NR_close, static_cast<std::uint64_t>(fd));
            ++closed;
        }
    }
    (void)closed;
    return restored;
}

void transfer_control(RestoreTarget& target, const image::ImageReader& image) {
    const auto r = image.registers();

    user_regs_struct regs{};
    regs.rip = r.rip;
    regs.rsp = r.rsp;
    regs.eflags = r.rflags;
    regs.rax = r.gpr[0];
    regs.rbx = r.gpr[1];
    regs.rcx = r.gpr[2];
    regs.rdx = r.gpr[3];
    regs.rsi = r.gpr[4];
    regs.rdi = r.gpr[5];
    regs.rbp = r.gpr[6];
    regs.r8 = r.gpr[7];
    regs.r9 = r.gpr[8];
    regs.r10 = r.gpr[9];
    regs.r11 = r.gpr[10];
    regs.r12 = r.gpr[11];
    regs.r13 = r.gpr[12];
    regs.r14 = r.gpr[13];
    regs.r15 = r.gpr[14];
    regs.fs_base = r.fs_base;
    regs.gs_base = r.gs_base;
    regs.cs = r.cs;
    regs.ss = r.ss;
    regs.ds = r.ds;
    regs.es = r.es;
    regs.fs = r.fs;
    regs.gs = r.gs;

    // The stub is parked in a restartable syscall. Left as-is, the kernel would
    // rewind rip by 2 on resume to re-execute it, landing two bytes below the
    // restored entry point. (docs/phase2-spike-report.md, trap 4.)
    regs.orig_rax = static_cast<unsigned long long>(-1);

    ::iovec iov{&regs, sizeof regs};
    if (::ptrace(PTRACE_SETREGSET, target.pid(), reinterpret_cast<void*>(NT_PRSTATUS), &iov) != 0)
        throw CheckpointError(std::string("restore: SETREGSET(NT_PRSTATUS): ") +
                              std::strerror(errno));

    if (!r.xstate.empty()) {
        // A 512-byte blob is the legacy FXSAVE fallback the capture side uses
        // when the kernel has no xstate regset; anything larger is XSAVE.
        const int nt = (r.xstate.size() == sizeof(user_fpregs_struct)) ? NT_PRFPREG
                                                                      : NT_X86_XSTATE;
        std::vector<std::byte> blob = r.xstate;
        ::iovec xiov{blob.data(), blob.size()};
        if (::ptrace(PTRACE_SETREGSET, target.pid(), reinterpret_cast<void*>(nt), &xiov) != 0)
            throw CheckpointError(std::string("restore: SETREGSET(extended state, ") +
                                  std::to_string(blob.size()) + " bytes): " +
                                  std::strerror(errno) +
                                  " -- the image may come from an incompatible CPU");
    }

    if (::ptrace(PTRACE_DETACH, target.pid(), nullptr, nullptr) != 0)
        throw CheckpointError(std::string("restore: PTRACE_DETACH: ") + std::strerror(errno));

    target.release();  // it is its own process now
}

ResumeResult resume(RestoreTarget& target, const image::ImageReader& image) {
    ResumeResult res;
    const auto before = live_fds(target.pid()).size();
    res.fds_restored = restore_fds(target, image);
    const auto after = live_fds(target.pid()).size();
    res.fds_closed = (before > after) ? (before - after) : 0;
    res.xstate_bytes = image.registers().xstate.size();
    res.residual_mappings = 2;  // the reserve and the stub's TLS; see the header
    transfer_control(target, image);
    return res;
}

}  // namespace adaptive_restore::restore
