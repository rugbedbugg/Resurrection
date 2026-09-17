#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>
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
    for (std::uint32_t i = 0; i < 4; ++i) w.add_page({kA + i * 4096, 0, 0, PageEncoding::Raw, 0}, page);
    for (std::uint32_t i = 0; i < 2; ++i) w.add_page({kB + i * 4096, 0, 0, PageEncoding::Raw, 0}, page);
    for (std::uint32_t i = 2; i < 4; ++i) w.add_page({kB + i * 4096, 0, 0, PageEncoding::Zero, 0}, {});
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

    // Helpers: how the provider describes what it actually did.
    const auto deferred = [] { return RegionExecution{EffectiveAction::FullyDeferred, {}}; };
    const auto eager    = [] { return RegionExecution{EffectiveAction::EagerWritten, {}}; };
    const auto partial  = [](std::initializer_list<std::uint32_t> populated) {
        RegionExecution e{EffectiveAction::PrefetchedPartial, std::vector<std::uint8_t>(1, 0)};
        for (auto p : populated) e.populated[p / 8] |= static_cast<std::uint8_t>(1u << (p % 8));
        return e;
    };

    // --- both regions fully deferred: every page observable ----------------
    {
        const std::vector<RegionExecution> ex{deferred(), deferred()};
        const auto obs = observe_regions(image, trace, ex);
        assert(obs.size() == 2);
        assert(obs[0].fully_observable() && obs[1].fully_observable());
        assert(obs[0].observable_count() == 4 && obs[0].touched_count() == 1);
        assert(obs[1].observable_count() == 4 && obs[1].touched_count() == 3);

        const auto hist = to_history(obs);
        assert(hist.size() == 2);
        assert(std::fabs(hist[0].fraction_accessed_avg - 0.25f) < 1e-6);
        assert(std::fabs(hist[1].fraction_accessed_avg - 0.75f) < 1e-6);
        assert(std::fabs(hist[1].zero_page_fraction - 0.5f) < 1e-6);
        assert(hist[0].restore_count == 1 && "one observation per restore");

        // The bitmap must name WHICH pages, not just how many (§9.5).
        assert(!hist[0].page_was_touched(0) && hist[0].page_was_touched(1) &&
               !hist[0].page_was_touched(2) && !hist[0].page_was_touched(3));
        assert(hist[1].page_was_touched(0) && hist[1].page_was_touched(1) &&
               hist[1].page_was_touched(2) && !hist[1].page_was_touched(3));
    }

    // --- an eager region is UNOBSERVABLE and must be left alone ------------
    // Its pages were present, so nothing faulted. Recording 0% would teach the
    // controller "never accessed" about a region it correctly loaded eagerly.
    {
        const std::vector<RegionExecution> ex{eager(), deferred()};
        const auto obs = observe_regions(image, trace, ex);
        assert(obs.size() == 1 && "an eagerly written region yields no observation");
        assert(obs[0].region_id == 1);
    }

    // --- a degenerated prefetch IS fully observable ------------------------
    // Requested StaticPrefetch, but with no usable history nothing was written,
    // so the provider reports FullyDeferred and every fault is real evidence.
    // Keying off the REQUESTED action discarded exactly this case.
    {
        const std::vector<RegionExecution> ex{deferred(), deferred()};
        const auto obs = observe_regions(image, trace, ex);
        assert(obs.size() == 2 && "a prefetch that wrote nothing is observable");
        assert(!to_history(obs).empty() && "and must reach the profile");
    }

    // --- a partially prefetched region: only absent pages are observable ---
    {
        // Pages 0 and 2 of region 0 were written before resume; 1 and 3 absent.
        const std::vector<RegionExecution> ex{partial({0, 2}), eager()};
        const auto obs = observe_regions(image, trace, ex);
        assert(obs.size() == 1);
        assert(obs[0].observable_count() == 2 && "only the absent pages carry evidence");
        assert(!obs[0].fully_observable());
        assert(obs[0].bit(obs[0].observable, 1) && obs[0].bit(obs[0].observable, 3));
        assert(!obs[0].bit(obs[0].observable, 0) && !obs[0].bit(obs[0].observable, 2));
        // Page 1 faulted, so it is positive evidence.
        assert(obs[0].bit(obs[0].touched, 1) && obs[0].touched_count() == 1);

        // A resident page that did not fault is UNKNOWN, never a negative.
        // v2 history has no way to say "unknown", so the region is not stored.
        std::uint32_t skipped = 0;
        const auto hist = to_history(obs, &skipped);
        assert(hist.empty() && skipped == 1 &&
               "partial evidence must not be flattened into a full bitmap");
    }

    // --- a region nobody touched is still observed, as zero ----------------
    {
        const std::vector<RegionExecution> ex{deferred(), deferred()};
        const auto obs = observe_regions(image, {}, ex);
        assert(obs.size() == 2);
        const auto hist = to_history(obs);
        assert(hist[0].fraction_accessed_avg == 0.0f &&
               "with every page absent, no faults genuinely means untouched");
    }

    // --- a repeated fault on one page must not inflate the fraction --------
    {
        std::vector<FaultRecord> dup{{kA, 0, 0, 0, 4096, 0}, {kA, 0, 0, 0, 4096, 0},
                                     {kA, 0, 0, 0, 4096, 0}};
        const std::vector<RegionExecution> ex{deferred(), deferred()};
        const auto obs = observe_regions(image, dup, ex);
        assert(obs[0].touched_count() == 1 && "distinct pages, not fault count");
    }

    std::puts("test_region_profiler: ok");
    return 0;
}
