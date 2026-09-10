#pragma once
#include <cstdint>

namespace adaptive_restore::policy {

// The three constants the Tier 1 cost model (§12) is arithmetic over.
// Calibrated once at startup, never continuously (continuous recalibration is
// Tier 2). Measured values and their spread: docs/phase4-calibration-report.md.
struct Calibration {
    double sequential_bandwidth_bps = 0;  // bytes/s, sequential read of the image
    double effective_bandwidth_bps = 0;   // bytes/s, scattered prefetch reads
    double mean_fault_cost_s = 0;         // seconds per demand fault, as the TARGET sees it

    // Defaults measured on ext4/NVMe. Used when calibration is skipped; they
    // are a starting point, not a substitute for measuring this machine.
    static Calibration defaults_warm();
    static Calibration defaults_cold();

    // Measure this machine. `image_dir` must be on real storage: a directory on
    // tmpfs measures RAM and silently invalidates every cold-cache figure.
    static Calibration measure(const char* image_dir);

    // §12: C_eager(r) = bytes / sequential_bandwidth
    double cost_eager(std::uint64_t bytes) const;
    // §12: C_lazy(r) = expected_faults * mean_fault_cost
    double cost_lazy(std::uint64_t expected_faults) const;
    // §12: C_prefetch(r) = prefetch_bytes / effective_bandwidth
    //                    + miss_faults * mean_fault_cost
    //                    + lambda_waste * wasted_bytes
    double cost_prefetch(std::uint64_t prefetch_bytes, std::uint64_t miss_faults,
                         std::uint64_t wasted_bytes, double lambda_waste) const;
};

}  // namespace adaptive_restore::policy
