// plan_restore() is the single source of truth for "what will each region get".
// It was extracted out of restore_image() so `arctl visualize` could draw the
// real plan instead of a re-derived guess; these tests pin the properties both
// callers depend on.

#include <cassert>
#include <cstdio>
#include <vector>

#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/policy/plan.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::policy;

namespace {

constexpr std::uint64_t kAnon = 0x100000000000ULL;
constexpr std::uint64_t kFile = 0x200000000000ULL;
constexpr std::uint32_t kPage = 4096;

// One anonymous mapping of `anon_pages` pages and one file-backed mapping of
// four, carved into regions of `region_pages` each.
std::vector<std::byte> build(std::uint32_t anon_pages, std::uint32_t region_pages) {
    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = kPage;
    h.kernel_release = "test";
    w.set_header(h);
    w.set_registers(RegisterRecord{});

    const auto anon_id = w.add_mapping({0, MappingFlag::Read | MappingFlag::Write |
                                               MappingFlag::ContentsSaved,
                                        kAnon, kAnon + anon_pages * kPage, 0, 0, 0, "[heap]"});
    const auto file_id = w.add_mapping({0, MappingFlag::Read | MappingFlag::FileBacked |
                                               MappingFlag::ContentsSaved,
                                        kFile, kFile + 4 * kPage, 0, 7, 9, "/lib/x.so"});

    // Every page is zero-encoded, which keeps the image tiny while still giving
    // the planner a real page index to read.
    std::uint32_t next = 0;
    const auto carve = [&](std::uint32_t mapping_id, std::uint64_t base, std::uint32_t pages) {
        for (std::uint32_t p = 0; p < pages; ++p)
            w.add_page({base + p * kPage, 0, 0, PageEncoding::Zero, 0}, {});
        for (std::uint32_t lo = 0; lo < pages; lo += region_pages) {
            const std::uint32_t hi = std::min(lo + region_pages, pages);
            w.add_region({0, mapping_id, base + lo * kPage, base + hi * kPage, next + lo,
                          next + hi});
        }
        next += pages;
    };
    carve(anon_id, kAnon, anon_pages);
    carve(file_id, kFile, 4);
    return w.serialize_to_bytes();
}

std::size_t count(const RestorePlan& p, paging::RestoreAction a) {
    std::size_t n = 0;
    for (const auto x : p.actions) n += (x == a);
    return n;
}

}  // namespace

int main() {
    // --- a file-backed region is eager under every policy --------------------
    // userfaultfd MISSING mode does not apply to it, so no policy may defer it.
    {
        auto bytes = build(64, 16);
        auto image = ImageReader::from_bytes(bytes);
        for (const auto pol : {RestorePolicy::Lazy, RestorePolicy::Prefetch}) {
            const auto plan = plan_restore(image, {}, pol, false);
            assert(plan.actions.size() == image.regions().size());
            // 4 anon regions deferred, 1 file-backed region forced eager.
            assert(count(plan, paging::RestoreAction::Eager) == 1);
        }
        const auto eager = plan_restore(image, {}, RestorePolicy::Eager, false);
        assert(count(eager, paging::RestoreAction::Eager) == eager.actions.size());
        assert(eager.decisions.empty() && "only the cost policy explains itself");
    }

    // --- the cost model declines deferral it cannot pay for ------------------
    // A tiny image cannot save the fixed setup cost, so the honest plan is to
    // defer nothing. This is the do-no-harm property, and it must hold with no
    // history at all, which is the very first restore of any checkpoint.
    {
        auto bytes = build(8, 4);
        auto image = ImageReader::from_bytes(bytes);
        const auto plan = plan_restore(image, {}, RestorePolicy::Cost, false);
        assert(plan.deferral_declined);
        assert(count(plan, paging::RestoreAction::Eager) == plan.actions.size());
        assert(plan.decisions.empty() && "a discarded plan must not be reported as chosen");
        assert(plan.predicted_transfer_s > 0.0 &&
               "the prediction must describe the plan that runs, not the one dropped");
    }

    // --- history turns a sparse region into a deferred one -------------------
    {
        auto bytes = build(4096, 64);   // 16 MiB of heap, worth deferring
        auto image = ImageReader::from_bytes(bytes);

        const auto cold = plan_restore(image, {}, RestorePolicy::Cost, false);

        // Now say every region was almost untouched last time.
        std::vector<profile::RegionHistory> history;
        for (const auto& r : image.regions()) {
            profile::RegionHistory h;
            h.region_id = r.region_id;
            h.restore_count = 5;
            h.fraction_accessed_avg = 0.02f;
            h.touched_bitmap.assign(8, 0);
            h.touched_bitmap[0] = 0x01;   // one page of each region
            history.push_back(h);
        }
        const auto warmed = plan_restore(image, history, RestorePolicy::Cost, false);
        assert(count(warmed, paging::RestoreAction::Eager) <
               count(cold, paging::RestoreAction::Eager) &&
               "evidence that a region is barely touched must move it off eager");
        assert(!warmed.deferral_declined);
        assert(!warmed.decisions.empty());
        for (const auto& d : warmed.decisions)
            assert(d.predicted_cost <= d.cost_eager + 1e-9 &&
                   "the chosen arm must be no worse than eager, or the argmin is wrong");
    }

    // --- features come from the image, not from assumptions ------------------
    {
        auto bytes = build(64, 16);
        auto image = ImageReader::from_bytes(bytes);
        const auto regions = image.regions();
        const auto mappings = image.mappings();
        const auto pages = image.page_index();

        const auto anon = features_for(regions.front(), mappings, pages, kPage, nullptr);
        assert(!anon.file_backed && anon.stack_or_heap && anon.pages == 16);
        assert(anon.zero_page_fraction == 1.0f);
        assert(anon.restore_count == 0 && "no history must be visible as no history");

        const auto file = features_for(regions.back(), mappings, pages, kPage, nullptr);
        assert(file.file_backed && !file.stack_or_heap);
    }

    std::puts("test_plan: ok");
    return 0;
}
