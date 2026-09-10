#pragma once
#include <cstdint>

namespace adaptive_restore::paging {

// What a region's pages cost at restore time. Frozen early (design-draft-v2
// §6.1) so Phase 4's controllers drop in without touching the data plane.
enum class RestoreAction {
    Eager,           // write the region's pages before resuming
    StaticPrefetch,  // write a recorded subset before resuming, fault the rest
    Lazy,            // write nothing; every page arrives as a demand fault
    // FaultReadAhead  // Tier 2, once read-ahead exists
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
