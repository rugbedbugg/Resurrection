#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "adaptive_restore/common/version.hpp"
#include "adaptive_restore/policy/calibration.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::policy;

int main() {
    // --- the cost formulas, with the bandwidth each path achieves -----------
    Calibration c;
    c.writev_bandwidth_bps = 1e9;      // 1 GB/s, eager's process_vm_writev
    c.uffd_copy_bandwidth_bps = 5e8;   // 0.5 GB/s, prefetch's UFFDIO_COPY
    c.mean_fault_cost_s = 25e-6;       // 25 us
    c.register_cost_s = 2e-6;          // 2 us per region

    assert(c.cost_eager(1000000) == 1e-3);                    // 1 MB at 1 GB/s
    assert(c.cost_lazy(40) == 2e-6 + 40 * 25e-6);             // registration + faults
    const double p = c.cost_prefetch(500000, 10, 100000, 1e-9);
    assert(p == 2e-6 + 500000 / 5e8 + 10 * 25e-6 + 1e-9 * 100000);

    // Deferring a region is never free: it costs a registration even if nothing
    // is prefetched and nothing is ever touched.
    assert(c.cost_lazy(0) == 2e-6);
    assert(c.cost_eager(kDefaultRegionBytes) > c.cost_lazy(0));

    // The measured defaults must price each transfer path on its own figure:
    // pricing prefetch with read bandwidth is what made the controller choose
    // it and then lose on wall time. Which path is faster per byte is a
    // property of the machine (it flips with the THP mode), not an invariant.
    const auto m = Calibration::defaults_cold();
    assert(m.writev_bandwidth_bps > 0 && m.uffd_copy_bandwidth_bps > 0);
    assert(m.writev_bandwidth_bps != m.sequential_bandwidth_bps &&
           m.uffd_copy_bandwidth_bps != m.sequential_bandwidth_bps &&
           "neither transfer path may be priced with read bandwidth");
    assert(m.register_cost_s > 0);

    // --- the crossover must sit inside a region, not outside it ------------
    // ImageReader loads the whole image at open under every policy, so the read
    // bandwidth cancels and cannot inform a decision. What differentiates is
    // the transfer path, which does not depend on cache state.
    {
        const auto k = Calibration::defaults_cold();
        const double crossover_pages = k.cost_eager(kDefaultRegionBytes) / k.mean_fault_cost_s;
        assert(crossover_pages > 1 && crossover_pages < 64 &&
               "lazy must win for lightly-touched regions and lose for dense ones");

        // Prefetching a known page must beat faulting for it: a fault is a
        // round trip, a prefetch is a copy.
        const double one_page_prefetch = k.cost_prefetch(4096, 0, 0, 0.0);
        assert(one_page_prefetch < k.cost_lazy(1) &&
               "if faulting were cheaper than copying, prefetch could never pay");
    }

    // --- measuring must not silently produce nonsense ----------------------
    const char* dir = std::getenv("AR_TEST_CALIB_DIR");
    if (dir) {
        const auto meas = Calibration::measure(dir);
        assert(meas.sequential_bandwidth_bps > 0);
        assert(meas.mean_fault_cost_s > 0);
    }

    std::puts("test_calibration: ok");
    return 0;
}
