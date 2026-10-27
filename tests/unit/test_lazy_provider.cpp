#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "adaptive_restore/checkpoint/proc_mem.hpp"
#include "adaptive_restore/common/error.hpp"
#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/paging/lazy_provider.hpp"
#include "adaptive_restore/restore/target.hpp"
#include "adaptive_restore/restore/uffd.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::paging;
using namespace adaptive_restore::restore;

static constexpr std::uint64_t kReserve = 0x700000000000ULL;
static constexpr std::size_t kReserveBytes = 4 * 4096;
static constexpr std::uint64_t kLazy = 0x100000000000ULL;

static std::byte pat(std::size_t i) { return static_cast<std::byte>((i * 7 + 3) & 0xff); }

int main() {
    // --- an image with one lazy region: a pattern page and a zero page -------
    std::vector<std::byte> page0(4096);
    for (std::size_t i = 0; i < 4096; ++i) page0[i] = pat(i);

    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = 4096;
    h.kernel_release = "test";
    w.set_header(h);
    w.set_registers(RegisterRecord{});
    w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                   kLazy, kLazy + 2 * 4096, 0, 0, 0, ""});
    w.add_page({kLazy, 0, 0, PageEncoding::Raw, 0}, page0);
    w.add_page({kLazy + 4096, 0, 0, PageEncoding::Zero, 0}, {});
    w.add_region({0, 0, kLazy, kLazy + 2 * 4096, 0, 2});
    auto bytes = w.serialize_to_bytes();
    auto image = ImageReader::from_bytes(bytes);

    // The locator must find pages by any address inside them, not just the base.
    {
        PageLocator loc(image);
        assert(loc.find(kLazy) == 0);
        assert(loc.find(kLazy + 17) == 0 && "an interior address must resolve to its page");
        assert(loc.find(kLazy + 4096) == 1);
        assert(loc.find(kLazy - 1) == PageLocator::npos);
        assert(loc.find(kLazy + 2 * 4096) == PageLocator::npos);
        assert(loc.region_of(0) == 0 && loc.region_of(1) == 0);
    }

    assert(lazy_ranges(image).size() == 1);

    RestoreTarget target(kReserve, kReserveBytes);
    Userfault uffd(target);
    LazyProvider provider(image, uffd);

    // The region exists in the target but is deliberately left unpopulated.
    const long mapped = target.syscall(__NR_mmap, kLazy, 2 * 4096, PROT_READ | PROT_WRITE,
                                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                                       static_cast<std::uint64_t>(-1), 0);
    assert(static_cast<std::uint64_t>(mapped) == kLazy);

    provider.record_trace(true);
    provider.prepare_region(0, RestoreAction::Lazy);
    assert(provider.stats().regions_registered == 1);
    assert(provider.stats().pages_registered == 2);
    assert(uffd.registered_bytes() == 2 * 4096);

    // A syscall-triggered fault would return EFAULT rather than reaching the
    // handler under UFFD_USER_MODE_ONLY, so the
    // provider is driven directly here. UFFDIO_COPY/ZEROPAGE populate a
    // registered range whether or not a fault is pending, so this exercises the
    // real resolution path. Genuine demand faulting is covered end to end by
    // the lazy integration test, where the restored process runs its own code.
    PageRequest r0;
    r0.virtual_address = kLazy;
    r0.region_id = 0;
    provider.service_fault(r0);

    PageRequest r1;
    r1.virtual_address = kLazy + 4096 + 31;  // deliberately not page-aligned
    r1.region_id = 0;
    provider.service_fault(r1);

    assert(provider.stats().faults_served == 2);
    assert(provider.stats().copy_faults == 1);
    assert(provider.stats().zero_faults == 1);
    assert(provider.stats().bytes_supplied == 4096 && "only the raw page carries bytes");

    // What actually landed in the target must match the image byte for byte.
    checkpoint::ProcMem mem(target.pid());
    std::vector<std::byte> got;
    mem.read_range(kLazy, 4096, got);
    assert(std::memcmp(got.data(), page0.data(), 4096) == 0 &&
           "the served page must carry the image's bytes");
    mem.read_range(kLazy + 4096, 4096, got);
    assert(checkpoint::page_is_zero(got) && "the zero page must arrive zeroed");

    // The trace is the touched set Phase 4 measures waste and coverage against,
    // so it must record what was actually served, in order.
    const auto& tr = provider.trace();
    assert(tr.size() == 2);
    assert(tr[0].vaddr == kLazy && tr[1].vaddr == kLazy + 4096);
    assert(tr[0].bytes == 4096 && "a raw page reports the bytes supplied");
    assert(tr[1].bytes == 0 && "a zero page supplies no bytes from the image");
    assert(tr[0].encoding == static_cast<std::uint8_t>(PageEncoding::Raw));
    assert(tr[1].encoding == static_cast<std::uint8_t>(PageEncoding::Zero));
    assert(tr[1].offset_ns >= tr[0].offset_ns && "offsets must be monotonic");
    assert(provider.stats().service_ns_total > 0);

    // It must survive a round trip through the file the analysis reads.
    char trace_tmpl[] = "/tmp/artraceXXXXXX";
    const int tfd = ::mkstemp(trace_tmpl);
    assert(tfd >= 0);
    ::close(tfd);
    provider.write_trace(trace_tmpl);
    {
        std::ifstream f(trace_tmpl);
        std::string line;
        assert(std::getline(f, line) && line.rfind("vaddr\tregion", 0) == 0);
        int rows = 0;
        while (std::getline(f, line))
            if (!line.empty()) ++rows;
        assert(rows == 2 && "one row per served fault");
    }
    ::unlink(trace_tmpl);

    // A fault outside the image must be reported, not guessed at.
    bool threw = false;
    try {
        PageRequest bad;
        bad.virtual_address = 0x5000000000ULL;
        provider.service_fault(bad);
    } catch (const CheckpointError& e) {
        threw = std::string(e.what()).find("no page in the image") != std::string::npos;
    }
    assert(threw && "a fault with no matching page must throw");

    std::puts("test_lazy_provider: ok");
    return 0;
}
