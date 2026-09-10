#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adaptive_restore/checkpoint/maps.hpp"
#include "adaptive_restore/checkpoint/proc_mem.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/restore/address_space.hpp"
#include "adaptive_restore/restore/page_writer.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::restore;

static constexpr std::uint64_t kReserve = 0x700000000000ULL;
static constexpr std::size_t kReserveBytes = 4 * 4096;
static constexpr std::uint64_t kAnon = 0x100000000000ULL;   // 4 pages, rw
static constexpr std::uint64_t kRo = 0x100000010000ULL;     // 1 page, read-only
static constexpr std::uint64_t kFile = 0x100000020000ULL;   // 1 page, file-backed
static const char* kLibc = "/usr/lib/libc.so.6";

static std::byte pat(std::size_t i) { return static_cast<std::byte>((i * 31 + 7) & 0xff); }

int main() {
    struct stat libc_st {};
    assert(::stat(kLibc, &libc_st) == 0);

    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = 4096;
    h.kernel_release = "test";
    w.set_header(h);
    w.set_registers(RegisterRecord{});

    w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                   kAnon, kAnon + 4 * 4096, 0, 0, 0, ""});
    w.add_mapping({0, MappingFlag::Read | MappingFlag::ContentsSaved,
                   kRo, kRo + 4096, 0, 0, 0, ""});
    w.add_mapping({0, MappingFlag::Read | MappingFlag::FileBacked | MappingFlag::ContentsSaved,
                   kFile, kFile + 4096, 0, static_cast<std::uint64_t>(libc_st.st_dev),
                   static_cast<std::uint64_t>(libc_st.st_ino), kLibc});

    // Anonymous: two pattern pages, then a zero page, then another pattern page.
    // The zero page sits between them, so coalescing must break around it.
    std::vector<std::byte> page_a(4096), page_b(4096), page_d(4096);
    for (std::size_t i = 0; i < 4096; ++i) {
        page_a[i] = pat(i);
        page_b[i] = pat(i + 1000);
        page_d[i] = pat(i + 2000);
    }
    w.add_page({kAnon + 0 * 4096, 0, 0, PageEncoding::Raw, 0}, page_a);
    w.add_page({kAnon + 1 * 4096, 0, 0, PageEncoding::Raw, 0}, page_b);
    w.add_page({kAnon + 2 * 4096, 0, 0, PageEncoding::Zero, 0}, {});
    w.add_page({kAnon + 3 * 4096, 0, 0, PageEncoding::Raw, 0}, page_d);

    std::vector<std::byte> ro_page(4096, std::byte{0x5A});
    w.add_page({kRo, 0, 0, PageEncoding::Raw, 0}, ro_page);

    // The subtle one: a Zero page inside a FILE-BACKED mapping. The mapping is
    // created from libc, whose first page is a non-zero ELF header, so unless
    // the page is explicitly cleared the process would see stale file bytes.
    w.add_page({kFile, 0, 0, PageEncoding::Zero, 0}, {});

    auto bytes = w.serialize_to_bytes();
    auto image = ImageReader::from_bytes(bytes);

    auto target = make_target_for(image.mappings(), kReserve, kReserveBytes);
    rebuild_address_space(*target, image);

    // Prove the file-backed page really is non-zero before population, or the
    // assertion below would pass for the wrong reason.
    {
        checkpoint::ProcMem mem(target->pid());
        std::vector<std::byte> before;
        mem.read_range(kFile, 4096, before);
        assert(!checkpoint::page_is_zero(before) &&
               "libc's first page must be non-zero, otherwise this test proves nothing");
    }

    const auto res = populate_pages(*target, image, true);

    assert(res.verify_mismatches == 0);
    assert(res.pages_written == 4);        // 3 anon pattern + 1 read-only
    assert(res.pages_zeroed == 1);         // the file-backed zero page
    assert(res.pages_already_zero == 1);   // the anonymous zero page
    assert(res.bytes_written == 4 * 4096);
    assert(res.writev_calls < 6 && "contiguous runs must be coalesced");
    assert(res.mappings_protected == 2);   // read-only and file-backed; rw needs none

    // Read everything back independently of the verify pass.
    checkpoint::ProcMem mem(target->pid());
    std::vector<std::byte> got;

    mem.read_range(kAnon, 4096, got);
    assert(std::memcmp(got.data(), page_a.data(), 4096) == 0);
    mem.read_range(kAnon + 4096, 4096, got);
    assert(std::memcmp(got.data(), page_b.data(), 4096) == 0);
    mem.read_range(kAnon + 2 * 4096, 4096, got);
    assert(checkpoint::page_is_zero(got));
    mem.read_range(kAnon + 3 * 4096, 4096, got);
    assert(std::memcmp(got.data(), page_d.data(), 4096) == 0);

    mem.read_range(kRo, 4096, got);
    assert(got[0] == std::byte{0x5A} && got[4095] == std::byte{0x5A});

    // The whole point of pages_zeroed: stale file content must be gone.
    mem.read_range(kFile, 4096, got);
    assert(checkpoint::page_is_zero(got) &&
           "a Zero page in a file-backed mapping must be explicitly cleared");

    // Protections really were lowered in the target.
    bool ro_ok = false, rw_ok = false;
    for (const auto& m : checkpoint::read_maps(target->pid())) {
        if (m.start == kRo) ro_ok = m.r && !m.w && !m.x;
        if (m.start == kAnon) rw_ok = m.r && m.w;
    }
    assert(ro_ok && "the read-only mapping must end up read-only");
    assert(rw_ok);

    assert(target->syscall(__NR_getpid) == target->pid());

    // An omitted page cannot be supplied by an eager restore.
    {
        ImageWriter w2;
        w2.set_header(h);
        w2.set_registers(RegisterRecord{});
        w2.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                        kAnon, kAnon + 4096, 0, 0, 0, ""});
        w2.add_page({kAnon, 0, 0, PageEncoding::Omitted, 0}, {});
        auto b2 = w2.serialize_to_bytes();
        auto img2 = ImageReader::from_bytes(b2);
        auto t2 = make_target_for(img2.mappings(), kReserve, kReserveBytes);
        rebuild_address_space(*t2, img2);
        bool threw = false;
        try {
            populate_pages(*t2, img2, false);
        } catch (const CheckpointError& e) {
            threw = std::string(e.what()).find("omitted") != std::string::npos;
        }
        assert(threw && "an omitted page must fail an eager restore");
    }

    std::puts("test_page_writer: ok");
    return 0;
}
