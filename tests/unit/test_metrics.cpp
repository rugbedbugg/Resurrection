#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include "adaptive_restore/image/image_reader.hpp"
#include "adaptive_restore/image/image_writer.hpp"
#include "adaptive_restore/policy/metrics.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::image;
using namespace adaptive_restore::paging;
using namespace adaptive_restore::policy;
using namespace adaptive_restore::profile;

static constexpr std::uint64_t kA = 0x100000000000ULL;  // 8 pages, all touched
static constexpr std::uint64_t kB = 0x100000010000ULL;  // 8 pages, 1 touched
static constexpr std::uint64_t kC = 0x100000020000ULL;  // 8 pages, none touched

int main() {
    ImageWriter w;
    ImageHeaderInfo h{};
    h.page_size = 4096;
    h.kernel_release = "t";
    w.set_header(h);
    w.set_registers(RegisterRecord{});
    std::vector<std::byte> pg(4096, std::byte{1});
    std::uint32_t idx = 0;
    for (std::uint64_t base : {kA, kB, kC}) {
        w.add_mapping({0, MappingFlag::Read | MappingFlag::Write | MappingFlag::ContentsSaved,
                       base, base + 8 * 4096, 0, 0, 0, ""});
        for (std::uint32_t i = 0; i < 8; ++i) w.add_page({base + i * 4096, 0, 0, PageEncoding::Raw, 0}, pg);
        w.add_region({0, 0, base, base + 8 * 4096, idx, idx + 8});
        idx += 8;
    }
    auto bytes = w.serialize_to_bytes();
    auto image = ImageReader::from_bytes(bytes);

    std::vector<RegionHistory> hist(3);
    hist[0] = {0, 1, 1.0f, 0.0f, RestoreAction::Lazy, {0xFF}};        // all 8
    hist[1] = {1, 1, 0.125f, 0.0f, RestoreAction::Lazy, {0x01}};      // page 0 only
    hist[2] = {2, 1, 0.0f, 0.0f, RestoreAction::Lazy, {0x00}};        // none

    const auto k = Calibration::defaults_cold();
    const auto bound = oracle_bound(image, hist, k);

    // A region nobody touches means "load nothing", which is lazy. Filing it
    // under prefetch would misreport what the optimal plan does.
    assert(bound.regions_lazy >= 1);
    assert(bound.seconds > 0);

    // Loading everything must never beat, and usually loses to, the oracle.
    const std::vector<RestoreAction> all_eager(3, RestoreAction::Eager);
    const double eager_cost = plan_cost(image, hist, all_eager, k, 0.0);
    assert(eager_cost >= bound.seconds && "no plan can beat a true lower bound");
    assert(eager_cost > bound.seconds && "eager must pay for the untouched region");

    // Faulting for everything is worse still on the dense region.
    const std::vector<RestoreAction> all_lazy(3, RestoreAction::Lazy);
    assert(plan_cost(image, hist, all_lazy, k, 0.0) > bound.seconds);

    // A plan that matches the oracle's per-region choice must equal the bound.
    // Single-page -> prefetch, untouched -> lazy. Dense goes to whichever
    // transfer path is cheaper per byte here, which depends on the machine.
    const std::uint64_t dense_bytes = 8 * 4096;
    const auto dense = k.cost_eager(dense_bytes) <= k.cost_prefetch(dense_bytes, 0, 0, 0.0)
                           ? RestoreAction::Eager
                           : RestoreAction::StaticPrefetch;
    const std::vector<RestoreAction> smart{dense, RestoreAction::StaticPrefetch,
                                           RestoreAction::Lazy};
    const double smart_cost = plan_cost(image, hist, smart, k, 0.0);
    assert(std::fabs(smart_cost - bound.seconds) < 1e-9 &&
           "choosing the oracle's action per region must cost the oracle's price");

    // Waste and coverage, the paging metrics.
    PagingMetrics eager_m{24, 9, 15};   // loaded all 24, needed 9, wasted 15
    assert(std::fabs(eager_m.waste() - 15.0 / 24.0) < 1e-9);
    assert(std::fabs(eager_m.coverage() - 1.0) < 1e-9 && "eager needs nothing at fault time");

    PagingMetrics lazy_m{0, 9, 0};      // loaded nothing up front
    assert(lazy_m.waste() == 0.0 && "lazy cannot waste what it never loads");
    assert(lazy_m.coverage() == 0.0 && "and covers nothing before it is wanted");

    std::puts("test_metrics: ok");
    return 0;
}
