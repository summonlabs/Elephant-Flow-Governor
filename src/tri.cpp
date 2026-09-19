// Elephant Flow Governor - three valued reasoning rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/tri.hpp"

#include <iterator>

namespace efg {
namespace {

struct ReasonName {
    ReasonCode code;
    std::string_view name;
};

// Fixed table, ordered by the bit position so that rendering is deterministic.
constexpr ReasonName kReasonNames[] = {
    {ReasonCode::VolumeThresholdMet, "volume_threshold_met"},
    {ReasonCode::DurationThresholdMet, "duration_threshold_met"},
    {ReasonCode::SustainedRateThresholdMet, "sustained_rate_threshold_met"},
    {ReasonCode::ResourceShareThresholdMet, "resource_share_threshold_met"},
    {ReasonCode::PolicyRuleSatisfied, "policy_rule_satisfied"},
    {ReasonCode::HysteresisHeld, "hysteresis_held"},
    {ReasonCode::ExitThresholdNotReached, "exit_threshold_not_reached"},
    {ReasonCode::BelowVolumeThreshold, "below_volume_threshold"},
    {ReasonCode::BelowDurationThreshold, "below_duration_threshold"},
    {ReasonCode::BelowRateThreshold, "below_rate_threshold"},
    {ReasonCode::BelowShareThreshold, "below_share_threshold"},
    {ReasonCode::PolicyRuleNotSatisfied, "policy_rule_not_satisfied"},
    {ReasonCode::EvidenceMissing, "evidence_missing"},
    {ReasonCode::EvidenceStale, "evidence_stale"},
    {ReasonCode::EvidenceUnknownField, "evidence_unknown_field"},
    {ReasonCode::TelemetryGap, "telemetry_gap"},
    {ReasonCode::CapacityMissing, "capacity_missing"},
    {ReasonCode::CapacityStale, "capacity_stale"},
    {ReasonCode::PathUnknown, "path_unknown"},
    {ReasonCode::PathGenerationChanged, "path_generation_changed"},
    {ReasonCode::FlowGenerationChanged, "flow_generation_changed"},
    {ReasonCode::PolicyChanged, "policy_changed"},
    {ReasonCode::FlowCompleted, "flow_completed"},
    {ReasonCode::PolicyRuleIndeterminate, "policy_rule_indeterminate"},
    {ReasonCode::RevalidationRequired, "revalidation_required"},
    {ReasonCode::ProtectedObligation, "protected_obligation"},
    {ReasonCode::NonPreemptibleObligation, "non_preemptible_obligation"},
    {ReasonCode::ReservedCapacityBounded, "reserved_capacity_bounded"},
    {ReasonCode::ControlClassBounded, "control_class_bounded"},
    {ReasonCode::GovernanceAuthorized, "governance_authorized"},
    {ReasonCode::GovernanceSuppressed, "governance_suppressed"},
    {ReasonCode::GovernanceClampedToBounds, "governance_clamped_to_bounds"},
    {ReasonCode::ImpactBelowActionThreshold, "impact_below_action_threshold"},
    {ReasonCode::ContentionBelowActionThreshold, "contention_below_action_threshold"},
};

}  // namespace

std::string_view to_string(Tri value) noexcept {
    switch (value) {
        case Tri::False: return "false";
        case Tri::True: return "true";
        case Tri::Unknown: return "unknown";
    }
    return "unknown";
}

std::vector<std::string_view> reason_names(ReasonCode set) {
    std::vector<std::string_view> out;
    out.reserve(std::size(kReasonNames));
    for (const ReasonName& entry : kReasonNames) {
        if (has_reason(set, entry.code)) {
            out.push_back(entry.name);
        }
    }
    return out;
}

std::string render_reasons(ReasonCode set) {
    if (set == ReasonCode::None) {
        return "none";
    }
    std::string out;
    bool first = true;
    for (std::string_view name : reason_names(set)) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out.append(name);
    }
    // Anything set outside the known table is reported rather than silently lost.
    std::uint64_t known = 0;
    for (const ReasonName& entry : kReasonNames) {
        known |= static_cast<std::uint64_t>(entry.code);
    }
    if ((static_cast<std::uint64_t>(set) & ~known) != 0) {
        if (!out.empty()) {
            out.push_back(',');
        }
        out.append("unrecognized_reason_bits");
    }
    return out;
}

}  // namespace efg
