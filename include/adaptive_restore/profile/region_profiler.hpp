#pragma once
#include <cstdint>
#include <vector>

#include "adaptive_restore/paging/lazy_provider.hpp"
#include "adaptive_restore/paging/page_provider.hpp"
#include "adaptive_restore/profile/profile_store.hpp"

namespace adaptive_restore::image {
class ImageReader;
}

namespace adaptive_restore::profile {

// Turn one restore's demand-fault trace into per-region observations (§10.1).
//
// **Only lazy regions are observable.** Under lazy, every first touch faults, so
// the trace is exactly the touched set. An eager region's pages are already
// present, so nothing faults and the trace says nothing about them. Recording a
// zero for those would teach the controller "never accessed" and push it toward
// lazy next time on precisely the regions it correctly loaded eagerly. So
// regions prepared any other way are skipped, leaving their history untouched.
//
// `actions` is indexed by region id and says how each region was prepared.
std::vector<RegionHistory> observe_regions(const image::ImageReader& image,
                                           const std::vector<paging::FaultRecord>& trace,
                                           const std::vector<paging::RestoreAction>& actions);

}  // namespace adaptive_restore::profile
