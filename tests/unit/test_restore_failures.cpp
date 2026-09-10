// Failure tests (design-draft-v2 §25.4): every one of these must fail
// explicitly, and none may leave a partially-restored process alive.
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>

#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/restore/restore.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::restore;

static constexpr std::uint64_t kReserve = 0x700000000000ULL;
static constexpr std::uint64_t kAnon = 0x100000000000ULL;

// Nothing may be left running or unreaped. A stub that survived a failed
// restore would show up here as an un-waited child.
static void assert_no_stray_children(const char* what) {
    int st = 0;
    const pid_t p = ::waitpid(-1, &st, WNOHANG);
    if (p > 0) {
        std::fprintf(stderr, "%s: stray child %d survived\n", what, p);
        assert(false && "a failed restore must not leave a process behind");
    }
    assert(p < 0 && errno == ECHILD);
}

static std::vector<std::byte> image_with(const MappingRecord& m, PageEncoding enc,
                                         const std::vector<std::byte>& payload) {
    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = 4096;
    h.kernel_release = "test";
    w.set_header(h);
    w.set_registers(RegisterRecord{});
    w.add_mapping(m);
    std::uint32_t n = 0;
    for (std::uint64_t a = m.start_vaddr; a < m.end_vaddr; a += 4096, ++n)
        w.add_page({a, 0, 0, enc, 0}, enc == PageEncoding::Raw ? std::span<const std::byte>(payload)
                                                               : std::span<const std::byte>{});
    w.add_region({0, 0, m.start_vaddr, m.end_vaddr, 0, n});
    return w.serialize_to_bytes();
}

static bool fails_with(const std::vector<std::byte>& bytes, const char* needle) {
    try {
        auto img = ImageReader::from_bytes(bytes);
        RestoreOptions o;
        restore_image(img, o);
    } catch (const std::exception& e) {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

int main() {
    const MappingRecord anon{0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                             kAnon, kAnon + 4096, 0, 0, 0, ""};
    const std::vector<std::byte> page(4096, std::byte{0x33});

    // --- a corrupt image is rejected before anything is forked --------------
    {
        auto good = image_with(anon, PageEncoding::Raw, page);

        auto truncated = good;
        truncated.resize(truncated.size() - 32);
        bool threw = false;
        try {
            ImageReader::from_bytes(truncated);
        } catch (const ImageError&) {
            threw = true;
        }
        assert(threw && "a truncated image must be refused");
        assert_no_stray_children("truncated image");

        auto flipped = good;
        flipped[flipped.size() - 1] ^= std::byte{0xFF};
        threw = false;
        try {
            ImageReader::from_bytes(flipped);
        } catch (const ImageError&) {
            threw = true;
        }
        assert(threw && "a corrupted image must be refused");
        assert_no_stray_children("corrupted image");
    }

    // --- an unplaceable mapping fails cleanly -------------------------------
    {
        // Claims the address compel needs for its own scratch area, so the
        // MAP_FIXED_NOREPLACE placement cannot succeed.
        MappingRecord clash = anon;
        clash.start_vaddr = kReserve;
        clash.end_vaddr = kReserve + 4096;
        assert(fails_with(image_with(clash, PageEncoding::Raw, page), "could not map"));
        assert_no_stray_children("address conflict");
    }

    // --- a missing backing file fails ---------------------------------------
    {
        MappingRecord gone = anon;
        gone.flags |= MappingFlag::FileBacked;
        gone.path = "/nonexistent/ar-no-such-library.so";
        assert(fails_with(image_with(gone, PageEncoding::Raw, page), "backing file is gone"));
        assert_no_stray_children("missing backing file");
    }

    // --- a backing file that moved fails ------------------------------------
    {
        MappingRecord moved = anon;
        moved.flags |= MappingFlag::FileBacked;
        moved.path = "/usr/lib/libc.so.6";
        moved.dev = 0xdead;
        moved.ino = 0xbeef;
        assert(fails_with(image_with(moved, PageEncoding::Raw, page),
                          "changed since checkpoint"));
        assert_no_stray_children("changed backing file");
    }

    // --- an omitted page cannot be supplied eagerly -------------------------
    {
        assert(fails_with(image_with(anon, PageEncoding::Omitted, {}), "omitted"));
        assert_no_stray_children("omitted page");
    }

    // --- a valid image with nonsense registers restores, then crashes -------
    // The restore itself succeeds; the process faults. That must be reported as
    // a signal death, never as success.
    {
        ImageWriter w;
        ImageHeaderInfo h{};
        h.page_size = 4096;
        h.kernel_release = "test";
        w.set_header(h);
        RegisterRecord rr{};
        rr.rip = 0x1000;  // never mapped by this image
        rr.rsp = kAnon + 2048;
        rr.cs = 0x33;
        rr.ss = 0x2b;
        w.set_registers(rr);
        w.add_mapping(anon);
        w.add_page({kAnon, 0, 0, PageEncoding::Raw, 0}, page);
        w.add_region({0, 0, kAnon, kAnon + 4096, 0, 1});
        auto bytes = w.serialize_to_bytes();

        auto img = ImageReader::from_bytes(bytes);
        RestoreOptions o;
        const auto s = restore_image(img, o);
        int st = 0;
        assert(::waitpid(s.pid, &st, 0) == s.pid);
        assert(WIFSIGNALED(st) && "a process resumed at an unmapped rip must die by signal");
        assert(WTERMSIG(st) == SIGSEGV);
        assert_no_stray_children("crashing restore");
    }

    std::puts("test_restore_failures: ok");
    return 0;
}
