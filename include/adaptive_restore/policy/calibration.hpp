#pragma once
#include <cstdint>

namespace adaptive_restore::policy {

// The three constants the Tier 1 cost model (§12) is arithmetic over.
// Calibrated once at startup, never continuously (continuous recalibration is
// Tier 2). Measured values and their spread: docs/phase4-calibration-report.md.
struct Calibration {
    // What actually differentiates the policies is how bytes reach the target,
    // not how the image is read: ImageReader loads the whole file at open under
    // every policy, so that cost cancels out and cannot inform a decision.
    double writev_bandwidth_bps = 0;    // process_vm_writev, what eager pays
    double uffd_copy_bandwidth_bps = 0; // UFFDIO_COPY, what prefetch pays
    double mean_fault_cost_s = 0;       // per demand fault, as the TARGET sees it
    double register_cost_s = 0;         // UFFDIO_REGISTER, fixed per region

    // One-time cost of enabling deferral at all: creating the uffd in the
    // target, stealing it, arming the death signal, disabling THP on the
    // affected spans, and running a page server for the target's lifetime.
    // §12 has no term for this, which lets a controller defer two regions out
    // of 283 and pay the whole setup for a saving of nothing. Measured at
    // ~10.3 ms on this machine.
    double deferral_setup_s = 0;

    // Kept for reporting: the image read is a real cost, just a policy-neutral
    // one, and §23 still wants cold and warm stated with every result.
    double sequential_bandwidth_bps = 0;

    // Defaults measured on ext4/NVMe. Used when calibration is skipped; they
    // are a starting point, not a substitute for measuring this machine.
    static Calibration defaults_warm();
    static Calibration defaults_cold();

    // Measure this machine. `image_dir` must be on real storage: a directory on
    // tmpfs measures RAM and silently invalidates every cold-cache figure.
    static Calibration measure(const char* image_dir);

    // §12's three costs, with the bandwidths each mechanism actually achieves.
    // Deferring a region costs a registration whether or not anything is
    // prefetched into it, so lazy and prefetch both carry that term.
    double cost_eager(std::uint64_t bytes) const;
    double cost_lazy(std::uint64_t expected_faults) const;
    double cost_prefetch(std::uint64_t prefetch_bytes, std::uint64_t miss_faults,
                         std::uint64_t wasted_bytes, double lambda_waste) const;
};

}  // namespace adaptive_restore::policy
