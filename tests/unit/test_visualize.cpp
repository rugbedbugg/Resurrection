// The visualizer is a consumer (§19): it must render whatever the rest of the
// system produced without ever touching a process. These tests check that the
// renderers stay honest about the numbers they are given, since a chart that
// quietly misreports is worse than no chart.

#include <cassert>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "adaptive_restore/tui/render.hpp"

using namespace adaptive_restore;
using namespace adaptive_restore::tui;

static bool has(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

int main() {
    const Style plain{false, 92};

    // --- the comparison view ------------------------------------------------
    {
        // Deliberately even-sized and out of order: the median must be the mean
        // of the middle pair, not whatever happened to be parsed first.
        std::vector<BenchRow> rows;
        for (double ms : {40.0, 10.0, 30.0, 20.0})
            rows.push_back({"w", "eager", "warm", ms, 0, true});
        for (double ms : {9.0, 11.0, 13.0, 7.0})
            rows.push_back({"w", "cost", "warm", ms, 5, true});

        std::ostringstream os;
        render_policy_comparison(os, rows, plain);
        const auto out = os.str();
        assert(has(out, "eager        25.0 ms"));
        assert(has(out, "cost         10.0 ms"));
        // The winner is marked, and the loser carries the gap that follows from
        // the medians above: 25.0 / 10.0 - 1 = +150%.
        assert(has(out, "best"));
        assert(has(out, "+150%"));
    }

    // --- a wrong-output run must not be presented as a result ----------------
    {
        std::vector<BenchRow> rows{{"w", "lazy", "warm", 5.0, 0, false},
                                   {"w", "eager", "warm", 9.0, 0, true}};
        std::ostringstream os;
        render_policy_comparison(os, rows, plain);
        // A policy that produced the wrong answer being fastest is the one
        // result that must never be reported as a win without a warning.
        assert(has(os.str(), "WARNING"));
    }

    // --- warm and cold must not be averaged together -------------------------
    {
        std::vector<BenchRow> rows{{"w", "eager", "warm", 10.0, 0, true},
                                   {"w", "eager", "cold", 90.0, 0, true}};
        std::ostringstream os;
        render_policy_comparison(os, rows, plain);
        const auto out = os.str();
        assert(has(out, "10.0 ms") && has(out, "90.0 ms"));
        assert(!has(out, "50.0 ms") && "cache states must stay separate cases");
    }

    // --- the timeline --------------------------------------------------------
    {
        std::vector<FaultRow> faults;
        for (int i = 0; i < 100; ++i)
            faults.push_back({std::uint64_t{0x1000} * static_cast<std::uint64_t>(i), 0u,
                              static_cast<std::uint64_t>(i) * 1000000u, 2000u, 4096u, 0u});
        std::ostringstream os;
        render_fault_timeline(os, faults, plain);
        const auto out = os.str();
        assert(has(out, "100 faults"));
        assert(has(out, "2.0 us mean"));   // 2000 ns of service, every fault
        assert(has(out, "0.20 ms"));       // 100 x 2000 ns in the handler
    }

    // --- every fault in one instant is a division hazard, not a drawing one ---
    {
        std::vector<FaultRow> faults(4);  // all offset_ns == 0
        std::ostringstream os;
        render_fault_timeline(os, faults, plain);
        assert(has(os.str(), "4 faults"));
    }

    // --- nothing to draw is a statement, not a blank ------------------------
    {
        std::ostringstream os;
        render_fault_timeline(os, {}, plain);
        assert(has(os.str(), "no faults recorded"));

        std::ostringstream os2;
        render_policy_comparison(os2, {}, plain);
        assert(has(os2.str(), "no measurements"));
    }

    // --- colour is opt-in and must not leak into a redirected stream ---------
    {
        std::vector<BenchRow> rows{{"w", "eager", "warm", 1.0, 0, true}};
        std::ostringstream plain_os, color_os;
        render_policy_comparison(plain_os, rows, Style{false, 92});
        render_policy_comparison(color_os, rows, Style{true, 92});
        assert(!has(plain_os.str(), "\033["));
        assert(has(color_os.str(), "\033["));
    }

    std::puts("test_visualize: ok");
    return 0;
}
