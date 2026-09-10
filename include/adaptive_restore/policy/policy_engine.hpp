#pragma once
#include "adaptive_restore/policy/calibration.hpp"
#include "adaptive_restore/policy/region.hpp"

namespace adaptive_restore::policy {

// The control plane (§6.2). Tier 1 has one implementation; Tier 2 adds bandit
// and tiny-neural engines behind this same interface.
class PolicyEngine {
public:
    virtual ~PolicyEngine() = default;
    virtual RegionDecision choose(const RegionFeatures& features, const SystemState& state) = 0;
    virtual void update(const RegionFeatures& features, paging::RestoreAction action,
                        const RegionOutcome& outcome) = 0;
};

// The Tier 1 controller (§12): three analytical costs, take the argmin.
// Deterministic and explainable by construction; it learns nothing at decision
// time, only reads what the profiler recorded earlier.
class CostModelPolicyEngine : public PolicyEngine {
public:
    // lambda_waste prices a wasted prefetched byte, in SECONDS PER BYTE, on top
    // of the bandwidth already charged for reading it. Default: one over the
    // effective bandwidth, i.e. a wasted byte costs its read twice over, once
    // for the read and once for the bandwidth it denied something useful.
    // Set to 0 to make waste free and let bandwidth alone decide.
    explicit CostModelPolicyEngine(Calibration cold, Calibration warm,
                                   double lambda_waste = -1.0);

    RegionDecision choose(const RegionFeatures& features, const SystemState& state) override;
    void update(const RegionFeatures& features, paging::RestoreAction action,
                const RegionOutcome& outcome) override;

    double lambda_waste() const { return lambda_waste_; }
    void set_lambda_waste(double v) { lambda_waste_ = v; }

private:
    Calibration cold_, warm_;
    double lambda_waste_;
};

}  // namespace adaptive_restore::policy
