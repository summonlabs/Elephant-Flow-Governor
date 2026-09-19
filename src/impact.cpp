// Elephant Flow Governor - impact implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/impact.hpp"

namespace efg {

u64 max_threshold_value(const RuleSet& rules, ThresholdKind kind) {
    u64 best = 0;
    for (const RuleNode& node : rules.nodes) {
        if (node.kind == RuleKind::Threshold && node.threshold.kind == kind) {
            best = max_of(best, node.threshold.value);
        }
    }
    return best;
}

u64 ImpactAssessment::digest() const {
    Hasher hasher;
    hasher.write_bool(defined);
    hasher.write_u32(share_of_capacity);
    hasher.write_u32(share_of_available);
    hasher.write_u32(rate_pressure);
    hasher.write_u32(contention_pressure);
    hasher.write_u64(excess_rate);
    hasher.write_u64(excess_over_guarantee);
    hasher.write_u32(competing_flows);
    hasher.write_u32(competing_elephants);
    hasher.write_bool(protected_obligation_conflict);
    hasher.write_u32(severity);
    return hasher.finish();
}

ImpactAssessment compute_impact(const FlowMeasurements& measurements,
                                const PolicyDocument& policy,
                                BasisPoints contention_pressure,
                                u32 competing_elephants) {
    ImpactAssessment out;
    if (!measurements.evidence_present) {
        return out;
    }
    out.defined = true;
    out.share_of_capacity = measurements.share_of_capacity;
    out.share_of_available = measurements.share_of_available;
    out.competing_flows = measurements.competing_flows;
    out.competing_elephants = competing_elephants;
    out.contention_pressure =
        contention_pressure > kBasisPointsScale ? kBasisPointsScale : contention_pressure;

    const u64 rate_threshold = max_threshold_value(policy.enter_rule, ThresholdKind::SustainedRate);
    if (rate_threshold != 0 && measurements.sustained_rate > rate_threshold) {
        out.excess_rate = measurements.sustained_rate - rate_threshold;
    }

    if (rate_threshold != 0) {
        u64 pressure = 0;
        if (mul_div(measurements.sustained_rate, static_cast<u64>(kBasisPointsScale), rate_threshold,
                    pressure)) {
            out.rate_pressure =
                static_cast<BasisPoints>(pressure > kBasisPointsScale ? kBasisPointsScale : pressure);
        } else {
            out.rate_pressure = kBasisPointsScale;
        }
    } else if (measurements.evidence_contiguous && measurements.sustained_rate > 0) {
        // No declared rate threshold: the flow is not rate-pressured by policy.
        out.rate_pressure = 0;
    }

    if (measurements.capacity_known && measurements.resource_capacity != 0 &&
        measurements.resource_reserved != 0) {
        const BasisPoints reserved_share =
            to_basis_points(measurements.resource_reserved, measurements.resource_capacity);
        if (measurements.share_of_capacity > reserved_share) {
            const BasisPoints above = measurements.share_of_capacity - reserved_share;
            u64 excess = 0;
            if (apply_basis_points(measurements.sustained_rate, above, excess)) {
                out.excess_over_guarantee = excess;
            }
        }
    }

    out.protected_obligation_conflict =
        measurements.has_reservation || measurements.protection == ProtectionState::Protected ||
        measurements.protection == ProtectionState::NonPreemptible ||
        measurements.service_class == ServiceClass::Reserved ||
        measurements.service_class == ServiceClass::Control;

    // Fixed weights summing to ten: 4 share, 4 contention, 2 rate pressure.
    const u64 weighted = static_cast<u64>(out.share_of_available) * 4u +
                         static_cast<u64>(out.contention_pressure) * 4u +
                         static_cast<u64>(out.rate_pressure) * 2u;
    out.severity = static_cast<BasisPoints>(weighted / 10u);
    if (out.severity > kBasisPointsScale) {
        out.severity = kBasisPointsScale;
    }
    return out;
}

}  // namespace efg
