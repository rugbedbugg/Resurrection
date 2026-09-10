#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "adaptive_restore/checkpoint/maps.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/restore/address_space.hpp"
#include "adaptive_restore/restore/target.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::restore;

static constexpr std::uint64_t kReserve = 0x700000000000ULL;
static constexpr std::size_t kReserveBytes = 4 * 4096;

// Well clear of the reserve (0x7000...) and of any ASLR'd TLS (0x7f...).
static constexpr std::uint64_t kAnon = 0x100000000000ULL;
static constexpr std::uint64_t kGuard = 0x100000010000ULL;

static std::vector<std::byte> build_image(bool with_file_mapping) {
    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = 4096;
    h.kernel_release = "test";
    w.set_header(h);
    w.set_registers(RegisterRecord{});

    // A writable anonymous region carrying contents.
    w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                   kAnon, kAnon + 2 * 4096, 0, 0, 0, ""});
    // A no-access guard region: no contents, but it must still be recreated.
    w.add_mapping({0, 0, kGuard, kGuard + 4096, 0, 0, 0, ""});
    // A kernel mapping: recorded, never recreated.
    w.add_mapping({0, MappingFlag::Read | MappingFlag::KernelSpecial,
                   0x7ffff7fb6000ULL, 0x7ffff7fba000ULL, 0, 0, 0, "[vvar]"});

    if (with_file_mapping) {
        // Map this test binary itself, so dev/ino validation has a real target.
        struct stat st {};
        assert(::stat("/usr/lib/libc.so.6", &st) == 0);
        w.add_mapping({0, MappingFlag::Read | MappingFlag::FileBacked | MappingFlag::ContentsSaved,
                       0x100000020000ULL, 0x100000024000ULL, 0,
                       static_cast<std::uint64_t>(st.st_dev),
                       static_cast<std::uint64_t>(st.st_ino), "/usr/lib/libc.so.6"});
    }
    return w.serialize_to_bytes();
}

int main() {
    // --- layout is rebuilt exactly ------------------------------------------
    {
        auto bytes = build_image(true);
        auto image = ImageReader::from_bytes(bytes);
        auto target = make_target_for(image.mappings(), kReserve, kReserveBytes);

        const auto res = rebuild_address_space(*target, image);
        assert(res.evicted > 0 && "the stub's own mappings must be torn down");
        assert(res.skipped_kernel == 1 && "[vvar] must be recorded but not recreated");
        assert(res.created == 3 && "anon + guard + file mappings must all be recreated");

        const auto live = checkpoint::read_maps(target->pid());
        auto find = [&](std::uint64_t start) -> const checkpoint::Mapping* {
            for (const auto& m : live)
                if (m.start == start) return &m;
            return nullptr;
        };

        const auto* anon = find(kAnon);
        assert(anon && anon->end == kAnon + 2 * 4096);
        assert(anon->r && anon->w && !anon->shared);

        // The guard region is recreated even though it carries no contents.
        const auto* guard = find(kGuard);
        assert(guard && guard->end == kGuard + 4096);

        // The file mapping is genuinely file-backed, not faked with anonymous memory.
        const auto* filemap = find(0x100000020000ULL);
        assert(filemap && filemap->path == "/usr/lib/libc.so.6");
        assert(filemap->kind == checkpoint::MapKind::FileBacked);

        // The kernel mapping was not recreated at the image's address.
        assert(find(0x7ffff7fb6000ULL) == nullptr);

        // The reserve and TLS survived, or compel would be dead.
        assert(find(kReserve) != nullptr);
        assert(target->syscall(__NR_getpid) == target->pid());
    }

    // --- a changed backing file is refused ----------------------------------
    {
        ImageWriter w;
        ImageHeaderInfo h{};
        h.page_size = 4096;
        h.kernel_release = "test";
        w.set_header(h);
        w.set_registers(RegisterRecord{});
        // Deliberately wrong dev/ino for a file that does exist.
        w.add_mapping({0, MappingFlag::Read | MappingFlag::FileBacked | MappingFlag::ContentsSaved,
                       kAnon, kAnon + 4096, 0, 0xdead, 0xbeef, "/usr/lib/libc.so.6"});
        auto bytes = w.serialize_to_bytes();
        auto image = ImageReader::from_bytes(bytes);
        auto target = make_target_for(image.mappings(), kReserve, kReserveBytes);

        bool threw = false;
        try {
            rebuild_address_space(*target, image);
        } catch (const CheckpointError& e) {
            threw = std::string(e.what()).find("changed since checkpoint") != std::string::npos;
        }
        assert(threw && "a backing file whose dev/ino moved must be refused");
    }

    // --- a TLS collision is detected, not silently ignored ------------------
    {
        RestoreTarget probe(kReserve, kReserveBytes);
        // An image claiming exactly the stub's TLS range must be rejected.
        std::vector<MappingRecord> colliding(1);
        colliding[0].flags = MappingFlag::Read | MappingFlag::ContentsSaved;
        colliding[0].start_vaddr = probe.tls_start();
        colliding[0].end_vaddr = probe.tls_end();
        assert(collides_with_tls(probe, colliding));

        // A kernel mapping over the same range is not a collision: it is never
        // recreated, so it cannot conflict.
        std::vector<MappingRecord> kernel_only(1);
        kernel_only[0].flags = MappingFlag::KernelSpecial;
        kernel_only[0].start_vaddr = probe.tls_start();
        kernel_only[0].end_vaddr = probe.tls_end();
        assert(!collides_with_tls(probe, kernel_only));

        // make_target_for must give up rather than loop forever.
        bool gave_up = false;
        try {
            // Claiming a huge span guarantees every stub's TLS collides.
            std::vector<MappingRecord> everything(1);
            everything[0].flags = MappingFlag::Read | MappingFlag::ContentsSaved;
            everything[0].start_vaddr = 0x7f0000000000ULL;
            everything[0].end_vaddr = 0x800000000000ULL;
            make_target_for(everything, kReserve, kReserveBytes, 3);
        } catch (const CheckpointError&) {
            gave_up = true;
        }
        assert(gave_up && "an unplaceable image must fail, not spin");
    }

    std::puts("test_address_space: ok");
    return 0;
}
