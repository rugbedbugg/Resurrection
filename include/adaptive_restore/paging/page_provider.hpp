#pragma once
#include <cstdint>
#include <vector>

namespace adaptive_restore::paging {

// What a region's pages cost at restore time. Frozen early so Phase 4's
// controllers drop in without touching the data plane.
enum class RestoreAction {
    Eager,           // write the region's pages before resuming
    StaticPrefetch,  // write a recorded subset before resuming, fault the rest
    Lazy,            // write nothing; every page arrives as a demand fault
    // FaultReadAhead  // Tier 2, once read-ahead exists
};

// What the provider ACTUALLY did with a region, which is not always what the
// planner asked for: a StaticPrefetch region with no usable history writes
// nothing and behaves as pure lazy. Keeping that fact only in provider
// statistics is what let the profiler discard thousands of real observations,
// because it inferred observability from the *requested* action.
enum class EffectiveAction : std::uint8_t {
    EagerWritten,       // every page populated before resume
    PrefetchedPartial,  // a subset populated; the rest left absent
    FullyDeferred,      // nothing populated; the whole region left absent
};

// Which pages of one region carried evidence, and what the provider really did.
//
// A page that was resident before resume and did not fault is UNKNOWN, not
// untouched -- nothing observes a read of an already-present page. Recording a
// zero for it would teach the controller "never accessed" about precisely the
// pages prefetch got right. `observable()` is the guard against that.
struct RegionExecution {
    EffectiveAction effective = EffectiveAction::EagerWritten;
    // bit i = page i of this region was written before resume. Only consulted
    // for PrefetchedPartial; the other two states are decided by `effective`.
    std::vector<std::uint8_t> populated;

    bool populated_page(std::uint32_t i) const {
        const std::size_t byte = i / 8;
        return byte < populated.size() && (populated[byte] & (1u << (i % 8)));
    }

    // True when this page's execution can distinguish touched from untouched.
    bool observable(std::uint32_t i) const {
        switch (effective) {
        case EffectiveAction::EagerWritten:      return false;
        case EffectiveAction::FullyDeferred:     return true;
        case EffectiveAction::PrefetchedPartial: return !populated_page(i);
        }
        return false;
    }
};

struct PageRequest {
    std::uint64_t virtual_address = 0;
    std::uint64_t image_offset = 0;
    std::uint32_t region_id = 0;
};

// The data plane. One implementation per restoration mechanism; the control
// plane picks a RestoreAction per region and the provider carries it out.
class PageProvider {
public:
    virtual ~PageProvider() = default;

    // Do whatever `action` requires for this region before the target resumes.
    virtual void prepare_region(std::uint32_t region_id, RestoreAction action) = 0;

    // Supply one page on demand. Only reachable for actions that defer pages;
    // an eager provider treats a call here as a bug, because a fault means a
    // page it promised to have written was missing.
    virtual void service_fault(const PageRequest& request) = 0;
};

}  // namespace adaptive_restore::paging
