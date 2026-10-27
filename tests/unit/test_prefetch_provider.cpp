#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "adaptive_restore/checkpoint/proc_mem.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/paging/prefetch_provider.hpp"
#include "adaptive_restore/restore/address_space.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::paging;
using namespace adaptive_restore::profile;
using namespace adaptive_restore::restore;

static constexpr std::uint64_t kReserve = 0x700000000000ULL;
static constexpr std::size_t kReserveBytes = 4 * 4096;
static constexpr std::uint64_t kR = 0x100000000000ULL;   // 4 pages
static constexpr std::uint32_t kPages = 4;

static std::byte pat(std::uint32_t page, std::size_t i) {
    return static_cast<std::byte>(((page + 1) * 37 + i) & 0xff);
}

int main() {
    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = 4096;
    h.kernel_release = "t";
    w.set_header(h);
    w.set_registers(RegisterRecord{});
    w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                   kR, kR + kPages * 4096, 0, 0, 0, ""});
    std::vector<std::vector<std::byte>> content(kPages, std::vector<std::byte>(4096));
    for (std::uint32_t p = 0; p < kPages; ++p) {
        for (std::size_t i = 0; i < 4096; ++i) content[p][i] = pat(p, i);
        w.add_page({kR + p * 4096, 0, 0, PageEncoding::Raw, 0}, content[p]);
    }
    w.add_region({0, 0, kR, kR + kPages * 4096, 0, kPages});
    auto bytes = w.serialize_to_bytes();
    auto image = ImageReader::from_bytes(bytes);

    const auto build_target = [&]() {
        auto t = std::make_unique<RestoreTarget>(kReserve, kReserveBytes);
        const long m = t->syscall(__NR_mmap, kR, kPages * 4096, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                                  static_cast<std::uint64_t>(-1), 0);
        assert(static_cast<std::uint64_t>(m) == kR);
        return t;
    };

    // --- with a recorded working set: exactly that subset is loaded --------
    {
        auto target = build_target();
        Userfault uffd(*target);
        // History says pages 0 and 2 were touched last time.
        RegionHistory hist;
        hist.region_id = 0;
        hist.touched_bitmap = {0b00000101};
        PrefetchProvider p(image, *target, uffd, {hist});

        p.prepare_region(0, RestoreAction::StaticPrefetch);
        assert(p.stats().pages_prefetched == 2);
        assert(p.stats().regions_prefetched == 1);
        assert(p.stats().bytes_prefetched == 2 * 4096);
        assert(p.stats().regions_without_history == 0);

        // The prefetched pages carry the image's bytes...
        checkpoint::ProcMem mem(target->pid());
        std::vector<std::byte> got;
        mem.read_range(kR + 0 * 4096, 4096, got);
        assert(std::memcmp(got.data(), content[0].data(), 4096) == 0);
        mem.read_range(kR + 2 * 4096, 4096, got);
        assert(std::memcmp(got.data(), content[2].data(), 4096) == 0);

        // ...and the unprefetched ones are still MISSING, so they will fault
        // rather than silently reading as zero. That is the safety
        // fallback: a wrong prediction costs latency, never correctness.
        bool page1_missing = false;
        try {
            mem.read_range(kR + 1 * 4096, 4096, got);
        } catch (const CheckpointError&) {
            page1_missing = true;
        }
        assert(page1_missing && "an unprefetched page must remain handled, not resolved");

        // A fault on it is counted as a miss and served from the image.
        PageRequest miss;
        miss.virtual_address = kR + 1 * 4096;
        miss.region_id = 0;
        p.service_fault(miss);
        assert(p.stats().misses == 1);
        mem.read_range(kR + 1 * 4096, 4096, got);
        assert(std::memcmp(got.data(), content[1].data(), 4096) == 0);
    }

    // --- with no history: degrade to lazy, do not fail --------------------
    {
        auto target = build_target();
        Userfault uffd(*target);
        PrefetchProvider p(image, *target, uffd, {});
        p.prepare_region(0, RestoreAction::StaticPrefetch);
        assert(p.stats().pages_prefetched == 0);
        assert(p.stats().regions_without_history == 1 &&
               "a cold profile means a slow first run, not a broken one");

        // Every page is still handled, so nothing is lost.
        checkpoint::ProcMem mem(target->pid());
        std::vector<std::byte> got;
        bool missing = false;
        try {
            mem.read_range(kR, 4096, got);
        } catch (const CheckpointError&) {
            missing = true;
        }
        assert(missing);
    }

    // --- a non-prefetch action is refused ---------------------------------
    {
        auto target = build_target();
        Userfault uffd(*target);
        PrefetchProvider p(image, *target, uffd, {});
        bool threw = false;
        try {
            p.prepare_region(0, RestoreAction::Eager);
        } catch (const CheckpointError&) {
            threw = true;
        }
        assert(threw);
    }

    std::puts("test_prefetch_provider: ok");
    return 0;
}
