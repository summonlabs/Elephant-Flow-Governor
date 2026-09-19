// Elephant Flow Governor - policy implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/policy.hpp"

#include <algorithm>
#include <vector>

namespace efg {

std::string_view to_string(ThresholdKind kind) noexcept {
    switch (kind) {
        case ThresholdKind::Always: return "always";
        case ThresholdKind::Never: return "never";
        case ThresholdKind::CumulativeBytes: return "cumulative_bytes";
        case ThresholdKind::DurationTicks: return "duration_ticks";
        case ThresholdKind::SustainedRate: return "sustained_rate";
        case ThresholdKind::PeakRate: return "peak_rate";
        case ThresholdKind::ShareOfCapacity: return "share_of_capacity";
        case ThresholdKind::ShareOfAvailable: return "share_of_available";
        case ThresholdKind::CompetingFlows: return "competing_flows";
        case ThresholdKind::CompetingElephants: return "competing_elephants";
        case ThresholdKind::ExcessOverGuarantee: return "excess_over_guarantee";
        case ThresholdKind::ServiceClassIs: return "service_class_is";
        case ThresholdKind::PriorityAtLeast: return "priority_at_least";
        case ThresholdKind::IsProtected: return "is_protected";
        case ThresholdKind::IsNonPreemptible: return "is_non_preemptible";
        case ThresholdKind::HasReservation: return "has_reservation";
        case ThresholdKind::Count: return "count";
    }
    return "unknown";
}

bool parse_threshold_kind(std::string_view text, ThresholdKind& out) noexcept {
    for (u8 i = 0; i < static_cast<u8>(ThresholdKind::Count); ++i) {
        const auto kind = static_cast<ThresholdKind>(i);
        if (to_string(kind) == text) {
            out = kind;
            return true;
        }
    }
    return false;
}

std::string_view to_string(RuleKind kind) noexcept {
    switch (kind) {
        case RuleKind::Threshold: return "threshold";
        case RuleKind::All: return "all";
        case RuleKind::Any: return "any";
        case RuleKind::Not: return "not";
    }
    return "all";
}

Tri Threshold::evaluate(const FlowMeasurements& measurements) const {
    switch (kind) {
        case ThresholdKind::Always:
            return Tri::True;
        case ThresholdKind::Never:
            return Tri::False;
        case ThresholdKind::CumulativeBytes:
            if (!measurements.evidence_present) { return Tri::Unknown; }
            return tri_from_bool(measurements.cumulative_bytes >= value);
        case ThresholdKind::DurationTicks:
            if (!measurements.evidence_present) { return Tri::Unknown; }
            return tri_from_bool(measurements.duration_ticks >= value);
        case ThresholdKind::SustainedRate:
            // A rate is only meaningful over contiguous evidence: a gap or an
            // evicted window makes the ratio a lower bound, not a measurement.
            if (!measurements.evidence_present || !measurements.evidence_contiguous) {
                return Tri::Unknown;
            }
            return tri_from_bool(measurements.sustained_rate >= value);
        case ThresholdKind::PeakRate:
            if (!measurements.evidence_present) { return Tri::Unknown; }
            return tri_from_bool(measurements.peak_window_rate >= value);
        case ThresholdKind::ShareOfCapacity:
            if (!measurements.capacity_known) { return Tri::Unknown; }
            return tri_from_bool(measurements.share_of_capacity >= value);
        case ThresholdKind::ShareOfAvailable:
            if (!measurements.capacity_known) { return Tri::Unknown; }
            return tri_from_bool(measurements.share_of_available >= value);
        case ThresholdKind::CompetingFlows:
            if (!measurements.path_known) { return Tri::Unknown; }
            return tri_from_bool(static_cast<u64>(measurements.competing_flows) >= value);
        case ThresholdKind::CompetingElephants:
            if (!measurements.path_known) { return Tri::Unknown; }
            return tri_from_bool(static_cast<u64>(measurements.competing_elephants) >= value);
        case ThresholdKind::ExcessOverGuarantee:
            if (!measurements.capacity_known) { return Tri::Unknown; }
            return tri_from_bool(measurements.excess_over_guarantee >= value);
        case ThresholdKind::ServiceClassIs:
            if (measurements.service_class == ServiceClass::Unknown) { return Tri::Unknown; }
            return tri_from_bool(measurements.service_class == class_operand);
        case ThresholdKind::PriorityAtLeast:
            if (!measurements.priority.known()) { return Tri::Unknown; }
            return tri_from_bool(static_cast<u64>(measurements.priority.level()) >= value);
        case ThresholdKind::IsProtected:
            if (measurements.protection == ProtectionState::Unknown) { return Tri::Unknown; }
            return tri_from_bool(measurements.protection == ProtectionState::Protected ||
                                 measurements.protection == ProtectionState::NonPreemptible);
        case ThresholdKind::IsNonPreemptible:
            if (measurements.protection == ProtectionState::Unknown) { return Tri::Unknown; }
            return tri_from_bool(measurements.protection == ProtectionState::NonPreemptible);
        case ThresholdKind::HasReservation:
            return tri_from_bool(measurements.has_reservation);
        case ThresholdKind::Count:
            return Tri::Unknown;
    }
    return Tri::Unknown;
}

bool Threshold::observed(const FlowMeasurements& measurements, u64& out) const {
    switch (kind) {
        case ThresholdKind::CumulativeBytes: out = measurements.cumulative_bytes; return true;
        case ThresholdKind::DurationTicks: out = measurements.duration_ticks; return true;
        case ThresholdKind::SustainedRate: out = measurements.sustained_rate; return true;
        case ThresholdKind::PeakRate: out = measurements.peak_window_rate; return true;
        case ThresholdKind::ShareOfCapacity: out = measurements.share_of_capacity; return true;
        case ThresholdKind::ShareOfAvailable: out = measurements.share_of_available; return true;
        case ThresholdKind::CompetingFlows: out = measurements.competing_flows; return true;
        case ThresholdKind::CompetingElephants: out = measurements.competing_elephants; return true;
        case ThresholdKind::ExcessOverGuarantee: out = measurements.excess_over_guarantee; return true;
        case ThresholdKind::ServiceClassIs: out = static_cast<u64>(measurements.service_class); return true;
        case ThresholdKind::PriorityAtLeast:
            out = measurements.priority.known() ? measurements.priority.level() : 0;
            return measurements.priority.known();
        case ThresholdKind::IsProtected:
            out = static_cast<u64>(measurements.protection);
            return measurements.protection != ProtectionState::Unknown;
        case ThresholdKind::IsNonPreemptible:
            out = static_cast<u64>(measurements.protection);
            return measurements.protection != ProtectionState::Unknown;
        case ThresholdKind::HasReservation: out = measurements.has_reservation ? 1u : 0u; return true;
        case ThresholdKind::Always:
        case ThresholdKind::Never:
        case ThresholdKind::Count:
        default:
            return false;
    }
}

void ThresholdTrace::record(u32 node_index, const Threshold& threshold, Tri result,
                            const FlowMeasurements& measurements) {
    if (entries.size() >= limits::kMaxTraceEntries) {
        truncated = true;
        return;
    }
    ThresholdTraceEntry entry;
    entry.node_index = node_index;
    entry.kind = threshold.kind;
    entry.result = result;
    entry.threshold_value = threshold.value;
    entry.observed_defined = threshold.observed(measurements, entry.observed_value);
    entries.push_back(entry);
}

namespace {

struct EvalState {
    const RuleSet& rules;
    const FlowMeasurements& measurements;
    ThresholdTrace* trace;
    bool failed{false};
};

Tri evaluate_node(EvalState& state, u32 index, u32 depth) {
    if (state.failed) {
        return Tri::Unknown;
    }
    if (depth > limits::kMaxRuleDepth || index >= state.rules.nodes.size()) {
        state.failed = true;
        return Tri::Unknown;
    }
    const RuleNode& node = state.rules.nodes[index];
    switch (node.kind) {
        case RuleKind::Threshold: {
            const Tri result = node.threshold.evaluate(state.measurements);
            if (state.trace != nullptr) {
                state.trace->record(index, node.threshold, result, state.measurements);
            }
            return result;
        }
        case RuleKind::All: {
            Tri accumulated = Tri::True;
            for (const u32 child : node.children) {
                accumulated = tri_and(accumulated, evaluate_node(state, child, depth + 1));
            }
            return accumulated;
        }
        case RuleKind::Any: {
            Tri accumulated = Tri::False;
            for (const u32 child : node.children) {
                accumulated = tri_or(accumulated, evaluate_node(state, child, depth + 1));
            }
            return accumulated;
        }
        case RuleKind::Not: {
            return tri_not(evaluate_node(state, node.children[0], depth + 1));
        }
    }
    state.failed = true;
    return Tri::Unknown;
}

Status validate_node(const RuleSet& rules, u32 index, std::vector<u8>& colour, u32 depth) {
    if (index >= rules.nodes.size()) {
        return make_status(StatusCode::InvalidArgument, "rule node index is out of range");
    }
    if (depth > limits::kMaxRuleDepth) {
        return make_status(StatusCode::OutOfRange, "rule nesting exceeds the supported depth");
    }
    if (colour[index] == 1u) {
        return make_status(StatusCode::InvalidArgument, "rule graph contains a cycle");
    }
    if (colour[index] == 2u) {
        return {};
    }
    colour[index] = 1u;
    const RuleNode& node = rules.nodes[index];
    switch (node.kind) {
        case RuleKind::Threshold:
            if (!node.children.empty()) {
                return make_status(StatusCode::InvalidArgument, "threshold node must not have children");
            }
            if (node.threshold.kind >= ThresholdKind::Count) {
                return make_status(StatusCode::InvalidArgument, "threshold kind is not recognized");
            }
            break;
        case RuleKind::Not:
            if (node.children.size() != 1) {
                return make_status(StatusCode::InvalidArgument, "not node must have exactly one child");
            }
            break;
        case RuleKind::All:
        case RuleKind::Any:
            if (node.children.empty()) {
                return make_status(StatusCode::InvalidArgument, "composite node must have children");
            }
            break;
        default:
            return make_status(StatusCode::InvalidArgument, "rule kind is not recognized");
    }
    for (const u32 child : node.children) {
        EFG_TRY(validate_node(rules, child, colour, depth + 1));
    }
    colour[index] = 2u;
    return {};
}

}  // namespace

Status RuleSet::validate() const {
    if (nodes.empty()) {
        return make_status(StatusCode::InvalidArgument, "rule set is empty");
    }
    if (nodes.size() > limits::kMaxPolicyNodes) {
        return make_status(StatusCode::Oversized, "rule set exceeds the node ceiling");
    }
    if (root >= nodes.size()) {
        return make_status(StatusCode::InvalidArgument, "rule root index is out of range");
    }
    std::vector<u8> colour(nodes.size(), 0);
    return validate_node(*this, root, colour, 0);
}

Tri RuleSet::evaluate(const FlowMeasurements& measurements, ThresholdTrace* trace) const {
    if (nodes.empty() || root >= nodes.size()) {
        return Tri::Unknown;
    }
    EvalState state{*this, measurements, trace, false};
    const Tri result = evaluate_node(state, root, 0);
    if (state.failed) {
        return Tri::Unknown;
    }
    return result;
}

u64 RuleSet::digest() const {
    Hasher hasher;
    hasher.write_u64(root);
    hasher.write_u32(static_cast<u32>(nodes.size()));
    for (const RuleNode& node : nodes) {
        hasher.write_u8(static_cast<u8>(node.kind));
        hasher.write_u8(static_cast<u8>(node.threshold.kind));
        hasher.write_u64(node.threshold.value);
        hasher.write_u8(static_cast<u8>(node.threshold.class_operand));
        hasher.write_u32(static_cast<u32>(node.children.size()));
        for (const u32 child : node.children) {
            hasher.write_u32(child);
        }
    }
    return hasher.finish();
}

Status HysteresisSpec::validate() const {
    if (enter_confirm_windows == 0 || exit_confirm_windows == 0) {
        return make_status(StatusCode::InvalidArgument, "hysteresis confirmation counts must be positive");
    }
    if (enter_confirm_windows > 64 || exit_confirm_windows > 64) {
        return make_status(StatusCode::OutOfRange, "hysteresis confirmation counts are unreasonably large");
    }
    if (exit_scale_bp == 0 || exit_scale_bp > kBasisPointsScale) {
        return make_status(StatusCode::OutOfRange, "exit scale must be inside (0, 10000] basis points");
    }
    return {};
}

bool IntentAuthorization::permits(IntentKind kind) const noexcept {
    switch (kind) {
        case IntentKind::ObserveOnly:
        case IntentKind::ProtectReservedFlow:
            return true;
        case IntentKind::RequestAlternatePlacement:
            return allow_alternate_placement;
        case IntentKind::RequestRateShaping:
            return allow_rate_shaping;
        case IntentKind::RequestSchedulingIsolation:
            return allow_scheduling_isolation;
        case IntentKind::ReduceCompetingAdmission:
            return allow_competing_admission_reduction;
        case IntentKind::EscalateCongestion:
            return allow_congestion_escalation;
        case IntentKind::Count:
        default:
            return false;
    }
}

Status IntentAuthorization::validate() const {
    if (max_shaping_reduction_bp > kBasisPointsScale) {
        return make_status(StatusCode::OutOfRange, "shaping ceiling exceeds 10000 basis points");
    }
    if (max_intent_duration_ticks == 0) {
        return make_status(StatusCode::InvalidArgument, "intent lifetime must be positive");
    }
    if (max_targets == 0) {
        return make_status(StatusCode::InvalidArgument, "intent target ceiling must be positive");
    }
    if (min_impact_severity > kBasisPointsScale || min_contention_pressure > kBasisPointsScale) {
        return make_status(StatusCode::OutOfRange, "authorization thresholds exceed 10000 basis points");
    }
    return {};
}

Status PolicyDocument::validate() const {
    if (!id.valid()) {
        return make_status(StatusCode::InvalidArgument, "policy identifier is absent");
    }
    if (!generation.valid()) {
        return make_status(StatusCode::InvalidArgument, "policy generation is absent");
    }
    EFG_TRY(enter_rule.validate());
    if (!exit_rule.empty()) {
        EFG_TRY(exit_rule.validate());
    }
    EFG_TRY(hysteresis.validate());
    EFG_TRY(authorization.validate());
    if (!validity.well_formed()) {
        return make_status(StatusCode::InvalidArgument, "policy validity window is malformed");
    }
    if (!provenance.valid()) {
        return make_status(StatusCode::InvalidArgument, "policy provenance is incomplete");
    }
    if (provenance.schema_version != kSchemaVersion) {
        return make_status(StatusCode::VersionMismatch, "policy schema version is not supported");
    }
    if (digest == 0) {
        return make_status(StatusCode::InvalidArgument, "policy digest was never finalized");
    }
    return {};
}

Status PolicyDocument::finalize() {
    Hasher hasher;
    hasher.write_u64(id.value());
    hasher.write_u64(generation.value());
    hasher.write_digest(enter_rule.digest());
    hasher.write_digest(exit_rule.empty() ? 0ull : exit_rule.digest());
    hasher.write_u32(hysteresis.enter_confirm_windows);
    hasher.write_u32(hysteresis.exit_confirm_windows);
    hasher.write_u32(hysteresis.exit_scale_bp);
    hasher.write_bool(authorization.allow_alternate_placement);
    hasher.write_bool(authorization.allow_rate_shaping);
    hasher.write_bool(authorization.allow_scheduling_isolation);
    hasher.write_bool(authorization.allow_competing_admission_reduction);
    hasher.write_bool(authorization.allow_congestion_escalation);
    hasher.write_bool(authorization.protect_obligations);
    hasher.write_u32(authorization.max_shaping_reduction_bp);
    hasher.write_u64(authorization.max_intent_duration_ticks);
    hasher.write_u32(authorization.max_targets);
    hasher.write_u32(authorization.min_impact_severity);
    hasher.write_u32(authorization.min_contention_pressure);
    hasher.write_u64(validity.issued_at);
    hasher.write_u64(validity.expires_at);
    hasher.write_u64(provenance.publisher.publisher.value());
    hasher.write_u64(provenance.publisher.boot.value());
    hasher.write_u64(provenance.publisher.epoch.value());
    hasher.write_u32(provenance.schema_version);
    digest = hasher.finish();
    if (digest == 0) {
        digest = 1;  // never publish an absent digest
    }
    return {};
}

Tri PolicyDocument::evaluate_exit(const FlowMeasurements& measurements, ThresholdTrace* trace) const {
    if (!exit_rule.empty()) {
        return exit_rule.evaluate(measurements, trace);
    }
    const StatusOr<RuleSet> scaled = scale_rule_set(enter_rule, hysteresis.exit_scale_bp);
    if (!scaled.ok()) {
        return Tri::Unknown;
    }
    return scaled.value().evaluate(measurements, trace);
}

StatusOr<RuleSet> effective_exit_rule(const PolicyDocument& policy) {
    if (!policy.exit_rule.empty()) {
        return policy.exit_rule;
    }
    return scale_rule_set(policy.enter_rule, policy.hysteresis.exit_scale_bp);
}

StatusOr<RuleSet> scale_rule_set(const RuleSet& rules, BasisPoints scale_bp) {
    if (scale_bp == 0 || scale_bp > kBasisPointsScale) {
        return make_status(StatusCode::OutOfRange, "rule scale must be inside (0, 10000] basis points");
    }

    // The exit condition is built from the *instantaneous* thresholds of the
    // enter rule, scaled down. Monotone history thresholds cannot participate:
    // cumulative volume and elapsed duration never decrease, so leaving them in
    // a conjunction would make the exit rule unreachable and the flow would never
    // stop being an elephant. Class, priority and protection predicates are also
    // excluded, because an exit condition must not invent a weaker class test.
    RuleSet out;
    for (const RuleNode& node : rules.nodes) {
        if (node.kind != RuleKind::Threshold) {
            continue;
        }
        switch (node.threshold.kind) {
            case ThresholdKind::SustainedRate:
            case ThresholdKind::PeakRate:
            case ThresholdKind::ShareOfCapacity:
            case ThresholdKind::ShareOfAvailable:
            case ThresholdKind::ExcessOverGuarantee:
            case ThresholdKind::CompetingFlows:
            case ThresholdKind::CompetingElephants: {
                u64 scaled = 0;
                if (!mul_div(node.threshold.value, static_cast<u64>(scale_bp),
                             static_cast<u64>(kBasisPointsScale), scaled)) {
                    return make_status(StatusCode::Overflow, "scaled threshold overflowed");
                }
                Threshold threshold = node.threshold;
                threshold.value = scaled;
                out.nodes.push_back(RuleNode{RuleKind::Threshold, threshold, {}});
                break;
            }
            default:
                break;
        }
    }

    if (out.nodes.empty()) {
        // Nothing instantaneous to test: the flow leaves the elephant state as
        // soon as the enter rule stops holding.
        out.nodes.push_back(RuleNode{
            RuleKind::Threshold, Threshold{ThresholdKind::Always, 0, ServiceClass::Unknown}, {}});
        out.root = 0;
    } else {
        u32 chain = 0;
        // The leaf count is captured first: the chain nodes are appended to the
        // same array, so iterating against a live size would never terminate.
        const std::size_t leaves = out.nodes.size();
        for (std::size_t i = 1; i < leaves; ++i) {
            RuleNode node;
            node.kind = RuleKind::All;
            node.children.push_back(chain);
            node.children.push_back(static_cast<u32>(i));
            out.nodes.push_back(node);
            chain = static_cast<u32>(out.nodes.size() - 1);
        }
        out.root = chain;
    }
    EFG_TRY(out.validate());
    return out;
}

std::string_view describe_threshold(const Threshold& threshold) {
    switch (threshold.kind) {
        case ThresholdKind::CumulativeBytes: return "cumulative bytes at or above";
        case ThresholdKind::DurationTicks: return "duration in ticks at or above";
        case ThresholdKind::SustainedRate: return "sustained rate (bytes per 1000 ticks) at or above";
        case ThresholdKind::PeakRate: return "peak window rate (bytes per 1000 ticks) at or above";
        case ThresholdKind::ShareOfCapacity: return "share of resource capacity (basis points) at or above";
        case ThresholdKind::ShareOfAvailable: return "share of unreserved capacity (basis points) at or above";
        case ThresholdKind::CompetingFlows: return "competing flows on the binding resource at or above";
        case ThresholdKind::CompetingElephants: return "competing elephants on the binding resource at or above";
        case ThresholdKind::ExcessOverGuarantee: return "volume above the reserved band at or above";
        case ThresholdKind::ServiceClassIs: return "service class equals";
        case ThresholdKind::PriorityAtLeast: return "priority level at or above";
        case ThresholdKind::IsProtected: return "protection state is protected";
        case ThresholdKind::IsNonPreemptible: return "protection state is non-preemptible";
        case ThresholdKind::HasReservation: return "flow holds a reservation";
        case ThresholdKind::Always: return "unconditional";
        case ThresholdKind::Never: return "never";
        case ThresholdKind::Count: return "unrecognized";
    }
    return "unrecognized";
}

}  // namespace efg
