#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "adaptive_restore/policy/policy_engine.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::policy;
using paging::RestoreAction;

// A 256 KiB region, the Tier 1 granularity.
static RegionFeatures region(float fraction, std::uint32_t working_set, std::uint64_t restores) {
    RegionFeatures f;
    f.pages = 64;
    f.page_size = 4096;
    f.fraction_accessed_avg = fraction;
    f.recorded_working_set = working_set;
    f.restore_count = restores;
    return f;
}

int main() {
    // Real calibrated numbers, so the test exercises the boundary that actually
    // exists on this machine rather than a convenient invented one.
    CostModelPolicyEngine engine(Calibration::defaults_cold(), Calibration::defaults_warm());
    const SystemState cold{true};
    const SystemState warm{false};

    // --- §11.1 dense region: eager ----------------------------------------
    {
        const auto d = engine.choose(region(1.0f, 0, 10), cold);
        assert(d.action == RestoreAction::Eager);
        assert(d.cost_eager < d.cost_lazy && "reading 64 pages beats faulting 64 times");
        assert(d.confidence > 0.9f && "ten restores of history is confident");
    }

    // --- §11.3 sparse region: lazy ----------------------------------------
    {
        const auto d = engine.choose(region(0.02f, 0, 10), cold);
        assert(d.action == RestoreAction::Lazy);
        assert(d.cost_lazy < d.cost_eager);
    }

    // --- §11.2 stable useful subset: prefetch ------------------------------
    // 30% touched, and history says which 20 pages. Prefetch loads exactly
    // those, so it beats reading all 64 and beats faulting 19 times.
    {
        const auto d = engine.choose(region(0.30f, 20, 10), cold);
        assert(d.action == RestoreAction::StaticPrefetch);
        assert(d.cost_prefetch < d.cost_eager && d.cost_prefetch < d.cost_lazy);
    }

    // --- no history: assume dense, pick eager, admit low confidence --------
    {
        const auto d = engine.choose(region(0.01f, 40, 0), cold);
        assert(d.action == RestoreAction::Eager &&
               "with no history the recorded fraction is not evidence");
        assert(d.confidence == 0.0f);
        assert(std::isinf(d.cost_prefetch) && "nothing recorded means nothing to prefetch");
    }

    // --- cache state no longer changes the decision, and that is correct ---
    // ImageReader loads the whole image at open under every policy, so read
    // bandwidth cancels out. What differentiates the arms is how bytes reach
    // the target (process_vm_writev vs UFFDIO_COPY vs a fault round trip), and
    // none of that depends on the page cache.
    {
        const auto f = region(0.08f, 0, 10);
        const auto c = engine.choose(f, cold);
        const auto w = engine.choose(f, warm);
        assert(c.action == w.action &&
               "the transfer path is the same whether the image was cached or not");
        assert(c.cost_eager == w.cost_eager);
    }

    // --- the crossover sits where the measured costs put it ----------------
    // C_eager for a 256 KiB region is ~166us at 1505 MiB/s; a fault is ~26us.
    // So lazy should win below roughly 6 of 64 pages and lose above it.
    {
        assert(engine.choose(region(0.06f, 0, 10), cold).action == RestoreAction::Lazy);
        assert(engine.choose(region(0.20f, 0, 10), cold).action == RestoreAction::Eager);
    }

    // --- prefetching a known page must beat faulting for it ----------------
    // A fault is a round trip; a prefetch is a copy. If this inverted, static
    // prefetch could never pay for itself and §11.2 would be unreachable.
    {
        const auto known = engine.choose(region(0.10f, 6, 10), cold);
        const auto blind = engine.choose(region(0.10f, 0, 10), cold);
        assert(known.action == RestoreAction::StaticPrefetch);
        assert(known.predicted_cost < blind.predicted_cost &&
               "knowing which pages are needed must be worth something");
    }

    // --- the losing arms travel with the winner (§12 explainability) -------
    {
        const auto d = engine.choose(region(0.30f, 20, 3), cold);
        assert(d.cost_eager > 0 && d.cost_lazy > 0 && d.cost_prefetch > 0);
        assert(d.predicted_cost == std::min({d.cost_eager, d.cost_lazy, d.cost_prefetch}));
        assert(std::fabs(d.confidence - 0.6f) < 1e-6 && "three of five restores of history");
    }

    // --- lambda_waste is the one free parameter, and it must bite ----------
    {
        // A working set far larger than what is actually touched is mostly waste.
        const auto f = region(0.05f, 60, 10);
        engine.set_lambda_waste(0.0);
        const auto cheap = engine.choose(f, cold);
        engine.set_lambda_waste(1e-6);   // punitive: 1 us per wasted byte
        const auto dear = engine.choose(f, cold);
        assert(dear.cost_prefetch > cheap.cost_prefetch &&
               "pricing waste must make an over-broad prefetch less attractive");
        assert(dear.action != RestoreAction::StaticPrefetch);
    }

    std::puts("test_policy_engine: ok");
    return 0;
}
