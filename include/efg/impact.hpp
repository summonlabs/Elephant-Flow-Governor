// Elephant Flow Governor - impact assessment.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Impact answers "what harm is this flow creating right now", computed only from
// fresh, generation-bound evidence. Every quantity is integral so that impact is
// reproducible; the composite severity is a fixed weighted combination with
// weights summing to exactly ten, which keeps it inside 0..10000 basis points.

#ifndef EFG_IMPACT_HPP
#define EFG_IMPACT_HPP

#include "efg/checked.hpp"
#include "efg/flow.hpp"
#include "efg/hash.hpp"
#include "efg/policy.hpp"
#include "efg/tri.hpp"

namespace efg {

struct ImpactAssessment {
    bool defined{false};

    BasisPoints share_of_capacity{0};
    BasisPoints share_of_available{0};

    /// Sustained rate expressed relative to the policy's sustained rate threshold.
    BasisPoints rate_pressure{0};

    /// Sum of the capacity shares of competing elephant flows on the binding
    /// resource, saturating at 10000 basis points.
    BasisPoints contention_pressure{0};

    u64 excess_rate{0};
    u64 excess_over_guarantee{0};
    u32 competing_flows{0};
    u32 competing_elephants{0};

    /// True when protecting this flow constrains another protected obligation,
    /// for example when a flow's share is inside reserved capacity.
    bool protected_obligation_conflict{false};

    /// Composite severity in basis points. Rounding is floor and the weights are
    /// fixed at 4/10 share, 4/10 contention and 2/10 rate pressure.
    BasisPoints severity{0};

    [[nodiscard]] u64 digest() const;
};

void encode(ByteWriter& writer, const ImpactAssessment& impact);

/// Compute impact for a classified flow. Pure function of measurements, the
/// policy thresholds that produced the classification, and the elephant count on
/// the binding resource.
[[nodiscard]] ImpactAssessment compute_impact(const FlowMeasurements& measurements,
                                              const PolicyDocument& policy,
                                              BasisPoints contention_pressure,
                                              u32 competing_elephants);

/// Largest threshold value of the supplied kind declared anywhere in a rule set.
/// Deterministic because it scans the flattened node array in index order.
[[nodiscard]] u64 max_threshold_value(const RuleSet& rules, ThresholdKind kind);

}  // namespace efg

#endif  // EFG_IMPACT_HPP
