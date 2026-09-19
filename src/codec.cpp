// Elephant Flow Governor - canonical encoders and decoders.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/codec.hpp"

#include <string>

#include "efg/limits.hpp"

namespace efg {
namespace {

constexpr std::size_t kMaxStringBytes = limits::kMaxNameBytes;

[[nodiscard]] Status fail(const char* what) {
    return make_status(StatusCode::Truncated, what);
}

[[nodiscard]] Status oversize(const char* what) {
    return make_status(StatusCode::Oversized, what);
}

[[nodiscard]] Status bad_value(const char* what) {
    return make_status(StatusCode::Corrupt, what);
}

[[nodiscard]] Status read_count(ByteReader& reader, std::size_t ceiling, u32& out) {
    u32 count = 0;
    if (!reader.read_u32(count)) {
        return fail("encoded collection length is missing");
    }
    if (static_cast<std::size_t>(count) > ceiling) {
        return oversize("encoded collection length exceeds the ceiling");
    }
    out = count;
    return {};
}

template <typename Enum>
[[nodiscard]] Status read_enum(ByteReader& reader, u8 limit, Enum& out) {
    u8 raw = 0;
    if (!reader.read_u8(raw)) {
        return fail("encoded enumeration is missing");
    }
    if (raw >= limit) {
        return bad_value("encoded enumeration value is not recognized");
    }
    out = static_cast<Enum>(raw);
    return {};
}

}  // namespace

// --- Tick ------------------------------------------------------------------

void encode(ByteWriter& writer, const TickSpan& span) {
    writer.write_u64(span.begin);
    writer.write_u64(span.end);
}

Status decode(ByteReader& reader, TickSpan& span) {
    if (!reader.read_u64(span.begin) || !reader.read_u64(span.end)) {
        return fail("tick span is truncated");
    }
    return {};
}

void encode(ByteWriter& writer, const ValidityWindow& window) {
    writer.write_u64(window.issued_at);
    writer.write_u64(window.expires_at);
}

Status decode(ByteReader& reader, ValidityWindow& window) {
    if (!reader.read_u64(window.issued_at) || !reader.read_u64(window.expires_at)) {
        return fail("validity window is truncated");
    }
    return {};
}

void encode(ByteWriter& writer, const Provenance& provenance) {
    writer.write_u64(provenance.publisher.publisher.value());
    writer.write_u64(provenance.publisher.boot.value());
    writer.write_u64(provenance.publisher.epoch.value());
    writer.write_u64(provenance.publisher.sequence);
    writer.write_u64(provenance.emitted_at);
    writer.write_u32(provenance.schema_version);
    writer.write_u64(provenance.digest);
}

Status decode(ByteReader& reader, Provenance& provenance) {
    u64 publisher = 0;
    u64 boot = 0;
    u64 epoch = 0;
    if (!reader.read_u64(publisher) || !reader.read_u64(boot) || !reader.read_u64(epoch) ||
        !reader.read_u64(provenance.publisher.sequence) || !reader.read_u64(provenance.emitted_at) ||
        !reader.read_u32(provenance.schema_version) || !reader.read_u64(provenance.digest)) {
        return fail("provenance is truncated");
    }
    provenance.publisher.publisher = PublisherId{publisher};
    provenance.publisher.boot = BootId{boot};
    provenance.publisher.epoch = EpochId{epoch};
    return {};
}

// --- Policy ----------------------------------------------------------------

void encode(ByteWriter& writer, const Threshold& threshold) {
    writer.write_u8(static_cast<u8>(threshold.kind));
    writer.write_u64(threshold.value);
    writer.write_u8(static_cast<u8>(threshold.class_operand));
}

Status decode(ByteReader& reader, Threshold& threshold) {
    EFG_TRY(read_enum(reader, static_cast<u8>(ThresholdKind::Count), threshold.kind));
    if (!reader.read_u64(threshold.value)) {
        return fail("threshold value is truncated");
    }
    EFG_TRY(read_enum(reader, 6, threshold.class_operand));
    return {};
}

void encode(ByteWriter& writer, const RuleNode& node) {
    writer.write_u8(static_cast<u8>(node.kind));
    encode(writer, node.threshold);
    writer.write_u32(static_cast<u32>(node.children.size()));
    for (const u32 child : node.children) {
        writer.write_u32(child);
    }
}

Status decode(ByteReader& reader, RuleNode& node) {
    EFG_TRY(read_enum(reader, 4, node.kind));
    EFG_TRY(decode(reader, node.threshold));
    u32 count = 0;
    EFG_TRY(read_count(reader, limits::kMaxPolicyNodes, count));
    node.children.clear();
    node.children.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u32 child = 0;
        if (!reader.read_u32(child)) {
            return fail("rule child index is truncated");
        }
        node.children.push_back(child);
    }
    return {};
}

void encode(ByteWriter& writer, const RuleSet& rules) {
    writer.write_u32(static_cast<u32>(rules.nodes.size()));
    writer.write_u32(rules.root);
    for (const RuleNode& node : rules.nodes) {
        encode(writer, node);
    }
}

Status decode(ByteReader& reader, RuleSet& rules) {
    u32 count = 0;
    EFG_TRY(read_count(reader, limits::kMaxPolicyNodes, count));
    if (!reader.read_u32(rules.root)) {
        return fail("rule root index is truncated");
    }
    rules.nodes.clear();
    rules.nodes.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        RuleNode node;
        EFG_TRY(decode(reader, node));
        rules.nodes.push_back(std::move(node));
    }
    return {};
}

void encode(ByteWriter& writer, const HysteresisSpec& spec) {
    writer.write_u32(spec.enter_confirm_windows);
    writer.write_u32(spec.exit_confirm_windows);
    writer.write_u32(spec.exit_scale_bp);
}

Status decode(ByteReader& reader, HysteresisSpec& spec) {
    if (!reader.read_u32(spec.enter_confirm_windows) || !reader.read_u32(spec.exit_confirm_windows) ||
        !reader.read_u32(spec.exit_scale_bp)) {
        return fail("hysteresis specification is truncated");
    }
    return {};
}

void encode(ByteWriter& writer, const IntentAuthorization& authorization) {
    writer.write_bool(authorization.allow_alternate_placement);
    writer.write_bool(authorization.allow_rate_shaping);
    writer.write_bool(authorization.allow_scheduling_isolation);
    writer.write_bool(authorization.allow_competing_admission_reduction);
    writer.write_bool(authorization.allow_congestion_escalation);
    writer.write_bool(authorization.protect_obligations);
    writer.write_u32(authorization.max_shaping_reduction_bp);
    writer.write_u64(authorization.max_intent_duration_ticks);
    writer.write_u32(authorization.max_targets);
    writer.write_u32(authorization.min_impact_severity);
    writer.write_u32(authorization.min_contention_pressure);
}

Status decode(ByteReader& reader, IntentAuthorization& authorization) {
    if (!reader.read_bool(authorization.allow_alternate_placement) ||
        !reader.read_bool(authorization.allow_rate_shaping) ||
        !reader.read_bool(authorization.allow_scheduling_isolation) ||
        !reader.read_bool(authorization.allow_competing_admission_reduction) ||
        !reader.read_bool(authorization.allow_congestion_escalation) ||
        !reader.read_bool(authorization.protect_obligations) ||
        !reader.read_u32(authorization.max_shaping_reduction_bp) ||
        !reader.read_u64(authorization.max_intent_duration_ticks) ||
        !reader.read_u32(authorization.max_targets) ||
        !reader.read_u32(authorization.min_impact_severity) ||
        !reader.read_u32(authorization.min_contention_pressure)) {
        return fail("authorization envelope is truncated");
    }
    return {};
}

void encode(ByteWriter& writer, const PolicyDocument& policy) {
    writer.write_u64(policy.id.value());
    writer.write_u64(policy.generation.value());
    encode(writer, policy.enter_rule);
    encode(writer, policy.exit_rule);
    encode(writer, policy.hysteresis);
    encode(writer, policy.authorization);
    encode(writer, policy.validity);
    encode(writer, policy.provenance);
    writer.write_u64(policy.digest);
}

Status decode(ByteReader& reader, PolicyDocument& policy) {
    u64 id = 0;
    u64 generation = 0;
    if (!reader.read_u64(id) || !reader.read_u64(generation)) {
        return fail("policy identity is truncated");
    }
    policy.id = PolicyId{id};
    policy.generation = Generation{generation};
    EFG_TRY(decode(reader, policy.enter_rule));
    EFG_TRY(decode(reader, policy.exit_rule));
    EFG_TRY(decode(reader, policy.hysteresis));
    EFG_TRY(decode(reader, policy.authorization));
    EFG_TRY(decode(reader, policy.validity));
    EFG_TRY(decode(reader, policy.provenance));
    if (!reader.read_u64(policy.digest)) {
        return fail("policy digest is truncated");
    }
    return {};
}

// --- Evidence --------------------------------------------------------------

void encode(ByteWriter& writer, const FlowSample& sample) {
    writer.write_u64(sample.flow.value());
    writer.write_u64(sample.flow_generation.value());
    writer.write_u64(sample.path.value());
    writer.write_u64(sample.path_generation.value());
    writer.write_u64(sample.window.value());
    writer.write_u64(sample.window_sequence);
    encode(writer, sample.span);
    writer.write_u64(sample.cumulative_bytes);
    writer.write_u64(sample.window_bytes);
    writer.write_u8(static_cast<u8>(sample.service_class));
    writer.write_bool(sample.priority.known());
    writer.write_u8(sample.priority.level());
    writer.write_u8(static_cast<u8>(sample.protection));
    writer.write_u64(sample.reservation.value());
    writer.write_u64(sample.tenant.value());
    encode(writer, sample.validity);
    encode(writer, sample.provenance);
}

Status decode(ByteReader& reader, FlowSample& sample) {
    u64 flow = 0;
    u64 flow_generation = 0;
    u64 path = 0;
    u64 path_generation = 0;
    u64 window = 0;
    if (!reader.read_u64(flow) || !reader.read_u64(flow_generation) || !reader.read_u64(path) ||
        !reader.read_u64(path_generation) || !reader.read_u64(window) ||
        !reader.read_u64(sample.window_sequence)) {
        return fail("evidence identity is truncated");
    }
    sample.flow = FlowId{flow};
    sample.flow_generation = Generation{flow_generation};
    sample.path = PathId{path};
    sample.path_generation = Generation{path_generation};
    sample.window = EvidenceWindowId{window};
    EFG_TRY(decode(reader, sample.span));
    if (!reader.read_u64(sample.cumulative_bytes) || !reader.read_u64(sample.window_bytes)) {
        return fail("evidence volumes are truncated");
    }
    EFG_TRY(read_enum(reader, 6, sample.service_class));
    bool priority_known = false;
    if (!reader.read_bool(priority_known)) {
        return fail("evidence priority flag is truncated");
    }
    u8 level = 0;
    if (!reader.read_u8(level)) {
        return fail("evidence priority level is truncated");
    }
    if (priority_known && level > Priority::kMaxLevel) {
        return bad_value("evidence priority level is out of range");
    }
    sample.priority = priority_known ? Priority{level} : Priority::unknown();
    EFG_TRY(read_enum(reader, 4, sample.protection));
    u64 reservation = 0;
    u64 tenant = 0;
    if (!reader.read_u64(reservation) || !reader.read_u64(tenant)) {
        return fail("evidence obligation references are truncated");
    }
    sample.reservation = ReservationId{reservation};
    sample.tenant = TenantId{tenant};
    EFG_TRY(decode(reader, sample.validity));
    EFG_TRY(decode(reader, sample.provenance));
    return {};
}

void encode(ByteWriter& writer, const FlowMeasurements& measurements) {
    writer.write_bool(measurements.evidence_present);
    writer.write_bool(measurements.evidence_contiguous);
    writer.write_bool(measurements.gap_detected);
    writer.write_bool(measurements.capacity_known);
    writer.write_bool(measurements.path_known);
    encode(writer, measurements.observed);
    writer.write_u64(measurements.duration_ticks);
    writer.write_u64(measurements.cumulative_bytes);
    writer.write_u64(measurements.retained_window_bytes);
    writer.write_u64(measurements.sustained_rate);
    writer.write_u64(measurements.peak_window_rate);
    writer.write_u64(measurements.sample_count);
    writer.write_u64(measurements.window_count);
    writer.write_u64(measurements.dropped_windows);
    writer.write_u8(static_cast<u8>(measurements.service_class));
    writer.write_bool(measurements.priority.known());
    writer.write_u8(measurements.priority.level());
    writer.write_u8(static_cast<u8>(measurements.protection));
    writer.write_u64(measurements.reservation.value());
    writer.write_bool(measurements.has_reservation);
    writer.write_u64(measurements.tenant.value());
    writer.write_bool(measurements.completed);
    writer.write_u64(measurements.binding_resource.value());
    writer.write_u64(measurements.resource_capacity);
    writer.write_u64(measurements.resource_reserved);
    writer.write_u64(measurements.resource_available);
    writer.write_u32(measurements.share_of_capacity);
    writer.write_u32(measurements.share_of_available);
    writer.write_u32(measurements.competing_flows);
    writer.write_u32(measurements.competing_elephants);
    writer.write_u64(measurements.excess_over_guarantee);
    writer.write_bool(measurements.fields_complete);
}

Status decode(ByteReader& reader, FlowMeasurements& m) {
    if (!reader.read_bool(m.evidence_present) || !reader.read_bool(m.evidence_contiguous) ||
        !reader.read_bool(m.gap_detected) || !reader.read_bool(m.capacity_known) ||
        !reader.read_bool(m.path_known)) {
        return fail("measurement flags are truncated");
    }
    EFG_TRY(decode(reader, m.observed));
    if (!reader.read_u64(m.duration_ticks) || !reader.read_u64(m.cumulative_bytes) ||
        !reader.read_u64(m.retained_window_bytes) || !reader.read_u64(m.sustained_rate) ||
        !reader.read_u64(m.peak_window_rate) || !reader.read_u64(m.sample_count) ||
        !reader.read_u64(m.window_count) || !reader.read_u64(m.dropped_windows)) {
        return fail("measurement volumes are truncated");
    }
    EFG_TRY(read_enum(reader, 6, m.service_class));
    bool priority_known = false;
    if (!reader.read_bool(priority_known)) {
        return fail("measurement priority flag is truncated");
    }
    u8 level = 0;
    if (!reader.read_u8(level)) {
        return fail("measurement priority level is truncated");
    }
    if (priority_known && level > Priority::kMaxLevel) {
        return bad_value("measurement priority level is out of range");
    }
    m.priority = priority_known ? Priority{level} : Priority::unknown();
    EFG_TRY(read_enum(reader, 4, m.protection));
    u64 reservation = 0;
    u64 tenant = 0;
    u64 binding = 0;
    if (!reader.read_u64(reservation) || !reader.read_bool(m.has_reservation) ||
        !reader.read_u64(tenant) || !reader.read_bool(m.completed) ||
        !reader.read_u64(binding) || !reader.read_u64(m.resource_capacity) ||
        !reader.read_u64(m.resource_reserved) || !reader.read_u64(m.resource_available) ||
        !reader.read_u32(m.share_of_capacity) || !reader.read_u32(m.share_of_available) ||
        !reader.read_u32(m.competing_flows) || !reader.read_u32(m.competing_elephants) ||
        !reader.read_u64(m.excess_over_guarantee) || !reader.read_bool(m.fields_complete)) {
        return fail("measurement tail is truncated");
    }
    m.reservation = ReservationId{reservation};
    m.tenant = TenantId{tenant};
    m.binding_resource = ResourceId{binding};
    return {};
}

void encode(ByteWriter& writer, const ResourceCapacity& capacity) {
    writer.write_u64(capacity.resource.value());
    writer.write_u64(capacity.generation.value());
    writer.write_u64(capacity.snapshot.value());
    encode(writer, capacity.span);
    writer.write_u64(capacity.capacity);
    writer.write_u64(capacity.reserved);
    encode(writer, capacity.validity);
    encode(writer, capacity.provenance);
}

Status decode(ByteReader& reader, ResourceCapacity& capacity) {
    u64 resource = 0;
    u64 generation = 0;
    u64 snapshot = 0;
    if (!reader.read_u64(resource) || !reader.read_u64(generation) || !reader.read_u64(snapshot)) {
        return fail("capacity identity is truncated");
    }
    capacity.resource = ResourceId{resource};
    capacity.generation = Generation{generation};
    capacity.snapshot = CapacitySnapshotId{snapshot};
    EFG_TRY(decode(reader, capacity.span));
    if (!reader.read_u64(capacity.capacity) || !reader.read_u64(capacity.reserved)) {
        return fail("capacity volumes are truncated");
    }
    EFG_TRY(decode(reader, capacity.validity));
    EFG_TRY(decode(reader, capacity.provenance));
    return {};
}

void encode(ByteWriter& writer, const PathDescriptor& path) {
    writer.write_u64(path.path.value());
    writer.write_u64(path.generation.value());
    writer.write_u32(static_cast<u32>(path.resources.size()));
    for (const ResourceId resource : path.resources) {
        writer.write_u64(resource.value());
    }
    encode(writer, path.validity);
    encode(writer, path.provenance);
}

Status decode(ByteReader& reader, PathDescriptor& path) {
    u64 id = 0;
    u64 generation = 0;
    if (!reader.read_u64(id) || !reader.read_u64(generation)) {
        return fail("path identity is truncated");
    }
    path.path = PathId{id};
    path.generation = Generation{generation};
    u32 count = 0;
    EFG_TRY(read_count(reader, limits::kMaxResourcesPerPath, count));
    path.resources.clear();
    path.resources.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 resource = 0;
        if (!reader.read_u64(resource)) {
            return fail("path resource list is truncated");
        }
        path.resources.push_back(ResourceId{resource});
    }
    EFG_TRY(decode(reader, path.validity));
    EFG_TRY(decode(reader, path.provenance));
    return {};
}

// --- Decisions -------------------------------------------------------------

void encode(ByteWriter& writer, const ImpactAssessment& impact) {
    writer.write_bool(impact.defined);
    writer.write_u32(impact.share_of_capacity);
    writer.write_u32(impact.share_of_available);
    writer.write_u32(impact.rate_pressure);
    writer.write_u32(impact.contention_pressure);
    writer.write_u64(impact.excess_rate);
    writer.write_u64(impact.excess_over_guarantee);
    writer.write_u32(impact.competing_flows);
    writer.write_u32(impact.competing_elephants);
    writer.write_bool(impact.protected_obligation_conflict);
    writer.write_u32(impact.severity);
}

Status decode(ByteReader& reader, ImpactAssessment& impact) {
    if (!reader.read_bool(impact.defined) || !reader.read_u32(impact.share_of_capacity) ||
        !reader.read_u32(impact.share_of_available) || !reader.read_u32(impact.rate_pressure) ||
        !reader.read_u32(impact.contention_pressure) || !reader.read_u64(impact.excess_rate) ||
        !reader.read_u64(impact.excess_over_guarantee) || !reader.read_u32(impact.competing_flows) ||
        !reader.read_u32(impact.competing_elephants) ||
        !reader.read_bool(impact.protected_obligation_conflict) ||
        !reader.read_u32(impact.severity)) {
        return fail("impact assessment is truncated");
    }
    return {};
}

void encode(ByteWriter& writer, const AuthorityVector& authority) {
    writer.write_u64(authority.flow.value());
    writer.write_u64(authority.flow_generation.value());
    writer.write_u64(authority.path.value());
    writer.write_u64(authority.path_generation.value());
    writer.write_u64(authority.capacity.value());
    writer.write_u64(authority.capacity_generation.value());
    writer.write_u64(authority.window.value());
    writer.write_u64(authority.window_sequence);
    writer.write_u64(authority.policy.value());
    writer.write_u64(authority.policy_generation.value());
    writer.write_u64(authority.classification_generation.value());
    writer.write_u64(authority.epoch.value());
    writer.write_u64(authority.boot.value());
    writer.write_u64(authority.publisher.value());
    writer.write_u64(authority.publisher_boot.value());
    encode(writer, authority.validity);
    writer.write_u32(authority.bound_flags);
}

Status decode(ByteReader& reader, AuthorityVector& authority) {
    u64 flow = 0;
    u64 flow_generation = 0;
    u64 path = 0;
    u64 path_generation = 0;
    u64 capacity = 0;
    u64 capacity_generation = 0;
    u64 window = 0;
    u64 policy = 0;
    u64 policy_generation = 0;
    u64 classification_generation = 0;
    u64 epoch = 0;
    u64 boot = 0;
    u64 publisher = 0;
    u64 publisher_boot = 0;
    if (!reader.read_u64(flow) || !reader.read_u64(flow_generation) || !reader.read_u64(path) ||
        !reader.read_u64(path_generation) || !reader.read_u64(capacity) ||
        !reader.read_u64(capacity_generation) || !reader.read_u64(window) ||
        !reader.read_u64(authority.window_sequence) || !reader.read_u64(policy) ||
        !reader.read_u64(policy_generation) || !reader.read_u64(classification_generation) ||
        !reader.read_u64(epoch) || !reader.read_u64(boot) || !reader.read_u64(publisher) ||
        !reader.read_u64(publisher_boot)) {
        return fail("authority identity vector is truncated");
    }
    authority.flow = FlowId{flow};
    authority.flow_generation = Generation{flow_generation};
    authority.path = PathId{path};
    authority.path_generation = Generation{path_generation};
    authority.capacity = CapacitySnapshotId{capacity};
    authority.capacity_generation = Generation{capacity_generation};
    authority.window = EvidenceWindowId{window};
    authority.policy = PolicyId{policy};
    authority.policy_generation = Generation{policy_generation};
    authority.classification_generation = Generation{classification_generation};
    authority.epoch = EpochId{epoch};
    authority.boot = BootId{boot};
    authority.publisher = PublisherId{publisher};
    authority.publisher_boot = BootId{publisher_boot};
    EFG_TRY(decode(reader, authority.validity));
    if (!reader.read_u32(authority.bound_flags)) {
        return fail("authority binding flags are truncated");
    }
    return {};
}

void encode(ByteWriter& writer, const ThresholdTraceEntry& entry) {
    writer.write_u32(entry.node_index);
    writer.write_u8(static_cast<u8>(entry.kind));
    writer.write_u8(static_cast<u8>(entry.result));
    writer.write_bool(entry.observed_defined);
    writer.write_u64(entry.observed_value);
    writer.write_u64(entry.threshold_value);
}

Status decode(ByteReader& reader, ThresholdTraceEntry& entry) {
    if (!reader.read_u32(entry.node_index)) {
        return fail("threshold trace entry is truncated");
    }
    EFG_TRY(read_enum(reader, static_cast<u8>(ThresholdKind::Count), entry.kind));
    EFG_TRY(read_enum(reader, 3, entry.result));
    if (!reader.read_bool(entry.observed_defined) || !reader.read_u64(entry.observed_value) ||
        !reader.read_u64(entry.threshold_value)) {
        return fail("threshold trace values are truncated");
    }
    return {};
}

void encode(ByteWriter& writer, const FlowClassification& classification) {
    writer.write_u64(classification.id.value());
    writer.write_u64(classification.generation.value());
    writer.write_u64(classification.flow.value());
    writer.write_u64(classification.flow_generation.value());
    writer.write_u8(static_cast<u8>(classification.state));
    writer.write_u8(static_cast<u8>(classification.authority));
    encode(writer, classification.authority_vector);
    writer.write_u64(static_cast<u64>(classification.reasons));
    writer.write_u64(classification.decided_at);
    encode(writer, classification.validity);
    encode(writer, classification.measurements);
    encode(writer, classification.impact);
    writer.write_u32(static_cast<u32>(classification.trace.size()));
    for (const ThresholdTraceEntry& entry : classification.trace) {
        encode(writer, entry);
    }
    writer.write_bool(classification.trace_truncated);
    writer.write_u32(classification.enter_streak);
    writer.write_u32(classification.exit_streak);
    writer.write_u64(classification.history_digest);
    writer.write_u64(classification.digest);
}

Status decode(ByteReader& reader, FlowClassification& classification) {
    u64 id = 0;
    u64 generation = 0;
    u64 flow = 0;
    u64 flow_generation = 0;
    if (!reader.read_u64(id) || !reader.read_u64(generation) || !reader.read_u64(flow) ||
        !reader.read_u64(flow_generation)) {
        return fail("classification identity is truncated");
    }
    classification.id = ClassificationId{id};
    classification.generation = Generation{generation};
    classification.flow = FlowId{flow};
    classification.flow_generation = Generation{flow_generation};
    EFG_TRY(read_enum(reader, 4, classification.state));
    EFG_TRY(read_enum(reader, 3, classification.authority));
    EFG_TRY(decode(reader, classification.authority_vector));
    u64 reasons = 0;
    if (!reader.read_u64(reasons)) {
        return fail("classification reason mask is truncated");
    }
    classification.reasons = static_cast<ReasonCode>(reasons);
    if (!reader.read_u64(classification.decided_at)) {
        return fail("classification decision tick is truncated");
    }
    EFG_TRY(decode(reader, classification.validity));
    EFG_TRY(decode(reader, classification.measurements));
    EFG_TRY(decode(reader, classification.impact));
    u32 count = 0;
    EFG_TRY(read_count(reader, limits::kMaxTraceEntries, count));
    classification.trace.clear();
    classification.trace.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        ThresholdTraceEntry entry;
        EFG_TRY(decode(reader, entry));
        classification.trace.push_back(entry);
    }
    if (!reader.read_bool(classification.trace_truncated) ||
        !reader.read_u32(classification.enter_streak) ||
        !reader.read_u32(classification.exit_streak) ||
        !reader.read_u64(classification.history_digest) ||
        !reader.read_u64(classification.digest)) {
        return fail("classification tail is truncated");
    }
    return {};
}

void encode(ByteWriter& writer, const ClassificationTransition& transition) {
    writer.write_u64(transition.at);
    writer.write_u8(static_cast<u8>(transition.from));
    writer.write_u8(static_cast<u8>(transition.to));
    writer.write_u8(static_cast<u8>(transition.authority));
    writer.write_u64(static_cast<u64>(transition.reasons));
    writer.write_u64(transition.classification.value());
    writer.write_u64(transition.classification_generation.value());
}

Status decode(ByteReader& reader, ClassificationTransition& transition) {
    if (!reader.read_u64(transition.at)) {
        return fail("classification transition tick is truncated");
    }
    EFG_TRY(read_enum(reader, 4, transition.from));
    EFG_TRY(read_enum(reader, 4, transition.to));
    EFG_TRY(read_enum(reader, 3, transition.authority));
    u64 reasons = 0;
    u64 classification = 0;
    u64 generation = 0;
    if (!reader.read_u64(reasons) || !reader.read_u64(classification) ||
        !reader.read_u64(generation)) {
        return fail("classification transition tail is truncated");
    }
    transition.reasons = static_cast<ReasonCode>(reasons);
    transition.classification = ClassificationId{classification};
    transition.classification_generation = Generation{generation};
    return {};
}

void encode(ByteWriter& writer, const IntentBounds& bounds) {
    writer.write_u32(bounds.rate_reduction_bp);
    writer.write_u64(bounds.duration_ticks);
    writer.write_u32(bounds.targets);
    writer.write_bool(bounds.placement);
    writer.write_bool(bounds.isolation);
    writer.write_bool(bounds.admission_reduction);
    writer.write_bool(bounds.congestion_escalation);
}

Status decode(ByteReader& reader, IntentBounds& bounds) {
    if (!reader.read_u32(bounds.rate_reduction_bp) || !reader.read_u64(bounds.duration_ticks) ||
        !reader.read_u32(bounds.targets) || !reader.read_bool(bounds.placement) ||
        !reader.read_bool(bounds.isolation) || !reader.read_bool(bounds.admission_reduction) ||
        !reader.read_bool(bounds.congestion_escalation)) {
        return fail("intent bounds are truncated");
    }
    return {};
}

void encode(ByteWriter& writer, const GovernanceIntent& intent) {
    writer.write_u64(intent.id.value());
    writer.write_u64(intent.attempt.value());
    writer.write_u8(static_cast<u8>(intent.kind));
    writer.write_u64(intent.flow.value());
    writer.write_u64(intent.flow_generation.value());
    writer.write_u64(intent.classification.value());
    writer.write_u64(intent.classification_generation.value());
    writer.write_u8(static_cast<u8>(intent.state));
    writer.write_u32(static_cast<u32>(intent.suppressed));
    encode(writer, intent.requested);
    encode(writer, intent.granted);
    writer.write_u32(intent.severity);
    writer.write_u32(intent.contention_pressure);
    writer.write_u32(intent.competing_elephants);
    encode(writer, intent.window);
    encode(writer, intent.authority);
    encode(writer, intent.provenance);
    writer.write_u64(intent.digest);
}

Status decode(ByteReader& reader, GovernanceIntent& intent) {
    u64 id = 0;
    u64 attempt = 0;
    u64 flow = 0;
    u64 flow_generation = 0;
    u64 classification = 0;
    u64 classification_generation = 0;
    if (!reader.read_u64(id) || !reader.read_u64(attempt)) {
        return fail("intent identity is truncated");
    }
    EFG_TRY(read_enum(reader, static_cast<u8>(IntentKind::Count), intent.kind));
    if (!reader.read_u64(flow) || !reader.read_u64(flow_generation) ||
        !reader.read_u64(classification) || !reader.read_u64(classification_generation)) {
        return fail("intent bindings are truncated");
    }
    intent.id = IntentId{id};
    intent.attempt = AttemptId{attempt};
    intent.flow = FlowId{flow};
    intent.flow_generation = Generation{flow_generation};
    intent.classification = ClassificationId{classification};
    intent.classification_generation = Generation{classification_generation};
    EFG_TRY(read_enum(reader, 8, intent.state));
    u32 suppressed = 0;
    if (!reader.read_u32(suppressed)) {
        return fail("intent suppression reason is truncated");
    }
    intent.suppressed = static_cast<SuppressReason>(suppressed);
    EFG_TRY(decode(reader, intent.requested));
    EFG_TRY(decode(reader, intent.granted));
    if (!reader.read_u32(intent.severity) || !reader.read_u32(intent.contention_pressure) ||
        !reader.read_u32(intent.competing_elephants)) {
        return fail("intent impact figures are truncated");
    }
    EFG_TRY(decode(reader, intent.window));
    EFG_TRY(decode(reader, intent.authority));
    EFG_TRY(decode(reader, intent.provenance));
    if (!reader.read_u64(intent.digest)) {
        return fail("intent digest is truncated");
    }
    return {};
}

}  // namespace efg
