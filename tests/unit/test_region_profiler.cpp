#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/profile/region_profiler.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::paging;
using namespace adaptive_restore::profile;

static constexpr std::uint64_t kA = 0x100000000000ULL;  // 4 pages
static constexpr std::uint64_t kB = 0x100000010000ULL;  // 4 pages, half zero

int main() {
    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = 4096;
    h.kernel_release = "t";
    w.set_header(h);
    w.set_registers(RegisterRecord{});
    w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                   kA, kA + 4 * 4096, 0, 0, 0, ""});
    w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                   kB, kB + 4 * 4096, 0, 0, 0, ""});

    std::vector<std::byte> page(4096, std::byte{0x7});
    for (int i = 0; i < 4; ++i) w.add_page({kA + i * 4096, 0, 0, PageEncoding::Raw, 0}, page);
    for (int i = 0; i < 2; ++i) w.add_page({kB + i * 4096, 0, 0, PageEncoding::Raw, 0}, page);
    for (int i = 2; i < 4; ++i) w.add_page({kB + i * 4096, 0, 0, PageEncoding::Zero, 0}, {});
    w.add_region({0, 0, kA, kA + 4 * 4096, 0, 4});
    w.add_region({0, 1, kB, kB + 4 * 4096, 4, 8});

    auto bytes = w.serialize_to_bytes();
    auto image = ImageReader::from_bytes(bytes);

    // Region 0 saw one of its four pages; region 1 saw three of four.
    std::vector<FaultRecord> trace{
        {kA + 4096, 0, 0, 0, 4096, 0},
        {kB, 1, 0, 0, 4096, 0},
        {kB + 4096, 1, 0, 0, 4096, 0},
        {kB + 2 * 4096, 1, 0, 0, 0, 1},
    };

    // --- both regions lazy: both observable --------------------------------
    {
        const std::vector<RestoreAction> actions{RestoreAction::Lazy, RestoreAction::Lazy};
        const auto obs = observe_regions(image, trace, actions);
        assert(obs.size() == 2);
        assert(std::fabs(obs[0].fraction_accessed_avg - 0.25f) < 1e-6);
        assert(std::fabs(obs[1].fraction_accessed_avg - 0.75f) < 1e-6);
        assert(obs[0].zero_page_fraction == 0.0f);
        assert(std::fabs(obs[1].zero_page_fraction - 0.5f) < 1e-6);
        assert(obs[0].restore_count == 1 && "one observation per restore");
    }

    // --- an eager region is UNOBSERVABLE and must be left alone ------------
    // Its pages were present, so nothing faulted. Recording 0% would teach the
    // controller "never accessed" about a region it correctly loaded eagerly,
    // and push it toward lazy next time. That is the trap this guards.
    {
        const std::vector<RestoreAction> actions{RestoreAction::Eager, RestoreAction::Lazy};
        const auto obs = observe_regions(image, trace, actions);
        assert(obs.size() == 1 && "an eagerly prepared region yields no observation");
        assert(obs[0].region_id == 1);
    }
    {
        const std::vector<RestoreAction> actions{RestoreAction::StaticPrefetch,
                                                 RestoreAction::StaticPrefetch};
        assert(observe_regions(image, trace, actions).empty() &&
               "prefetched regions are partly present, so their access is not observable either");
    }

    // --- a region nobody touched is still observed, as zero ----------------
    {
        const std::vector<RestoreAction> actions{RestoreAction::Lazy, RestoreAction::Lazy};
        const auto obs = observe_regions(image, {}, actions);
        assert(obs.size() == 2);
        assert(obs[0].fraction_accessed_avg == 0.0f &&
               "under lazy, no faults genuinely means untouched");
    }

    // --- a repeated fault on one page must not inflate the fraction --------
    {
        std::vector<FaultRecord> dup{{kA, 0, 0, 0, 4096, 0}, {kA, 0, 0, 0, 4096, 0},
                                     {kA, 0, 0, 0, 4096, 0}};
        const std::vector<RestoreAction> actions{RestoreAction::Lazy, RestoreAction::Lazy};
        const auto obs = observe_regions(image, dup, actions);
        assert(std::fabs(obs[0].fraction_accessed_avg - 0.25f) < 1e-6 &&
               "distinct pages, not fault count");
    }

    std::puts("test_region_profiler: ok");
    return 0;
}
