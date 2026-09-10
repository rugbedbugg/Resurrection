#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adaptive_restore/checkpoint/registers.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/restore/address_space.hpp"
#include "adaptive_restore/restore/page_writer.hpp"
#include "adaptive_restore/restore/resume.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::restore;

static constexpr std::uint64_t kReserve = 0x700000000000ULL;
static constexpr std::size_t kReserveBytes = 4 * 4096;
static constexpr std::uint64_t kCode = 0x100000000000ULL;
static constexpr std::uint64_t kStack = 0x100000010000ULL;

// exit_group(42), so a successful control transfer is unambiguous.
static const unsigned char kShellcode[] = {
    0xb8, 0xe7, 0x00, 0x00, 0x00,  // mov eax, 231
    0xbf, 0x2a, 0x00, 0x00, 0x00,  // mov edi, 42
    0x0f, 0x05                     // syscall
};

// A real xstate blob for this CPU. A synthetic one would be rejected by the
// kernel, so the only honest way to exercise the path is to capture one.
static RegisterRecord real_register_shape() {
    const pid_t c = ::fork();
    assert(c >= 0);
    if (c == 0) {
        ::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        ::raise(SIGSTOP);
        _exit(0);
    }
    int st = 0;
    assert(::waitpid(c, &st, 0) == c);
    auto rr = checkpoint::capture_registers(c);
    ::ptrace(PTRACE_CONT, c, nullptr, nullptr);
    ::waitpid(c, &st, 0);
    return rr;
}

int main() {
    char tmpl[] = "/tmp/arresumeXXXXXX";
    const int seed = ::mkstemp(tmpl);
    assert(seed >= 0);
    assert(::ftruncate(seed, 8192) == 0);
    ::close(seed);
    struct stat fst {};
    assert(::stat(tmpl, &fst) == 0);

    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = 4096;
    h.kernel_release = "test";
    w.set_header(h);

    RegisterRecord rr = real_register_shape();
    rr.rip = kCode;
    rr.rsp = kStack + 4096 - 256;
    rr.fs_base = 0;  // the shellcode is freestanding; no TLS needed
    rr.gs_base = 0;
    w.set_registers(rr);

    w.add_mapping({0, MappingFlag::Read | MappingFlag::Exec | MappingFlag::ContentsSaved,
                   kCode, kCode + 4096, 0, 0, 0, ""});
    w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                   kStack, kStack + 4096, 0, 0, 0, ""});

    std::vector<std::byte> code(4096, std::byte{0});
    std::memcpy(code.data(), kShellcode, sizeof kShellcode);
    w.add_page({kCode, 0, 0, PageEncoding::Raw, 0}, code);
    w.add_page({kStack, 0, 0, PageEncoding::Zero, 0}, {});

    // fd 3 is a plain file at a known offset; fd 1 exercises the dup2-down path.
    w.add_fd({3, O_RDWR, 100, static_cast<std::uint64_t>(fst.st_dev),
              static_cast<std::uint64_t>(fst.st_ino), tmpl});
    w.add_fd({1, O_RDWR, 0, 0, 0, "/dev/null"});

    auto bytes = w.serialize_to_bytes();
    auto image = ImageReader::from_bytes(bytes);

    auto target = make_target_for(image.mappings(), kReserve, kReserveBytes);
    const pid_t pid = target->pid();
    rebuild_address_space(*target, image);
    const auto pop = populate_pages(*target, image, true);
    assert(pop.verify_mismatches == 0);

    // --- descriptors, checked while the target is still ours ----------------
    assert(restore_fds(*target, image) == 2);

    const auto link_of = [pid](int fd) {
        char buf[512] = {};
        const std::string p = "/proc/" + std::to_string(pid) + "/fd/" + std::to_string(fd);
        const ssize_t n = ::readlink(p.c_str(), buf, sizeof buf - 1);
        return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string();
    };
    assert(link_of(3) == tmpl);
    assert(link_of(1) == "/dev/null");
    // fd 0 and 2 were the stub's /dev/null and are not in the image, so they go.
    assert(link_of(0).empty() && link_of(2).empty());

    // The recorded seek offset was applied, not left at zero.
    {
        const std::string p = "/proc/" + std::to_string(pid) + "/fdinfo/3";
        FILE* f = std::fopen(p.c_str(), "r");
        assert(f);
        char line[256];
        long long pos = -1;
        while (std::fgets(line, sizeof line, f))
            if (std::strncmp(line, "pos:", 4) == 0) pos = std::atoll(line + 4);
        std::fclose(f);
        assert(pos == 100 && "the recorded file offset must be restored");
    }

    // --- hand it control ----------------------------------------------------
    transfer_control(*target, image);

    int st = 0;
    assert(::waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && "the restored process must run, not fault");
    assert(WEXITSTATUS(st) == 42 && "it must resume at the saved instruction pointer");

    ::unlink(tmpl);

    // --- a descriptor whose backing file moved must be refused --------------
    {
        ImageWriter w2;
        w2.set_header(h);
        w2.set_registers(RegisterRecord{});
        w2.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                        kCode, kCode + 4096, 0, 0, 0, ""});
        w2.add_page({kCode, 0, 0, PageEncoding::Zero, 0}, {});
        w2.add_fd({3, O_RDONLY, 0, 0xdead, 0xbeef, "/usr/lib/libc.so.6"});
        auto b2 = w2.serialize_to_bytes();
        auto img2 = ImageReader::from_bytes(b2);
        auto t2 = make_target_for(img2.mappings(), kReserve, kReserveBytes);
        rebuild_address_space(*t2, img2);
        bool threw = false;
        try {
            restore_fds(*t2, img2);
        } catch (const CheckpointError& e) {
            threw = std::string(e.what()).find("changed since checkpoint") != std::string::npos;
        }
        assert(threw && "an fd whose backing file moved must be refused");
    }

    std::puts("test_resume: ok");
    return 0;
}
