#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "adaptive_restore/common/version.hpp"
#include "adaptive_restore/policy/calibration.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::policy;

int main() {
    // --- the cost formulas are §12, verbatim -------------------------------
    Calibration c;
    c.sequential_bandwidth_bps = 1e9;   // 1 GB/s
    c.effective_bandwidth_bps = 5e8;    // 0.5 GB/s
    c.mean_fault_cost_s = 25e-6;        // 25 us

    assert(c.cost_eager(1000000) == 1e-3);          // 1 MB at 1 GB/s = 1 ms
    assert(c.cost_lazy(40) == 40 * 25e-6);          // 40 faults at 25 us
    // prefetch = bytes/effective + misses*fault + lambda*waste
    const double p = c.cost_prefetch(500000, 10, 100000, 1e-9);
    assert(p == 500000 / 5e8 + 10 * 25e-6 + 1e-9 * 100000);

    // A region nobody touches must never favour eager.
    assert(c.cost_lazy(0) == 0.0);
    assert(c.cost_eager(kDefaultRegionBytes) > c.cost_lazy(0));

    // --- the decision boundary must actually move with cache state ---------
    // This is the calibration report's headline: warm, eager wins almost
    // unconditionally; cold, lazy becomes right for lightly-touched regions.
    const auto warm = Calibration::defaults_warm();
    const auto cold = Calibration::defaults_cold();
    assert(warm.sequential_bandwidth_bps > cold.sequential_bandwidth_bps);

    const auto crossover_pages = [](const Calibration& k) {
        return k.cost_eager(kDefaultRegionBytes) / k.mean_fault_cost_s;
    };
    const double warm_x = crossover_pages(warm);
    const double cold_x = crossover_pages(cold);
    assert(warm_x < cold_x && "a cold cache must make lazy attractive over more of the region");
    assert(warm_x < 4 && "warm, only a nearly untouched region should prefer lazy");
    assert(cold_x > 4 && cold_x < 64 &&
           "cold, the crossover should sit inside a 64-page region, not outside it");

    // --- measuring must not silently produce nonsense ----------------------
    const char* dir = std::getenv("AR_TEST_CALIB_DIR");
    if (dir) {
        const auto m = Calibration::measure(dir);
        assert(m.sequential_bandwidth_bps > 0);
        assert(m.mean_fault_cost_s > 0);
    }

    std::puts("test_calibration: ok");
    return 0;
}
