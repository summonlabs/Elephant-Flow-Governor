// Elephant Flow Governor - explanation rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/explain.hpp"

#include <string>

#include "efg/text.hpp"

namespace efg {
namespace {

constexpr char kNewline = static_cast<char>(10);

}  // namespace

std::string_view to_string(ExplanationKind kind) noexcept {
    switch (kind) {
        case ExplanationKind::Qualification: return "qualification";
        case ExplanationKind::Disqualification: return "disqualification";
        case ExplanationKind::Indeterminate: return "indeterminate";
        case ExplanationKind::Suspension: return "suspension";
        case ExplanationKind::Protection: return "protection";
        case ExplanationKind::Authority: return "authority";
        case ExplanationKind::Impact: return "impact";
        case ExplanationKind::Intent: return "intent";
        case ExplanationKind::Suppression: return "suppression";
        case ExplanationKind::History: return "history";
    }
    return "qualification";
}

u64 Explanation::digest() const {
    Hasher hasher;
    hasher.write_u64(flow.value());
    hasher.write_u64(flow_generation.value());
    hasher.write_bool(found);
    hasher.write_u8(static_cast<u8>(state));
    hasher.write_u8(static_cast<u8>(authority));
    hasher.write_u64(static_cast<u64>(reasons));
    hasher.write_digest(measurements.digest());
    hasher.write_digest(impact.digest());
    hasher.write_u32(static_cast<u32>(trace.size()));
    for (const ThresholdTraceEntry& entry : trace) {
        hasher.write_u32(entry.node_index);
        hasher.write_u8(static_cast<u8>(entry.kind));
        hasher.write_u8(static_cast<u8>(entry.result));
        hasher.write_bool(entry.observed_defined);
        hasher.write_u64(entry.observed_value);
        hasher.write_u64(entry.threshold_value);
    }
    hasher.write_digest(authority_vector.digest());
    hasher.write_u64(history_digest);
    hasher.write_u64(history_folded);
    hasher.write_u32(static_cast<u32>(intents.size()));
    for (const GovernanceIntent& intent : intents) {
        hasher.write_u64(intent.digest);
    }
    return hasher.finish();
}

std::string render_authority_vector(const AuthorityVector& authority) {
    std::string out;
    out.reserve(192);
    out.append("flow=");
    append_u64(out, authority.flow.value());
    out.append("/gen=");
    append_u64(out, authority.flow_generation.value());
    out.append(" path=");
    append_u64(out, authority.path.value());
    out.append("/gen=");
    append_u64(out, authority.path_generation.value());
    out.append(" capacity=");
    append_u64(out, authority.capacity.value());
    out.append("/gen=");
    append_u64(out, authority.capacity_generation.value());
    out.append(" window=");
    append_u64(out, authority.window.value());
    out.append("/seq=");
    append_u64(out, authority.window_sequence);
    out.append(" policy=");
    append_u64(out, authority.policy.value());
    out.append("/gen=");
    append_u64(out, authority.policy_generation.value());
    out.append(" classification_gen=");
    append_u64(out, authority.classification_generation.value());
    out.append(" epoch=");
    append_u64(out, authority.epoch.value());
    out.append(" boot=");
    append_u64(out, authority.boot.value());
    out.append(" publisher=");
    append_u64(out, authority.publisher.value());
    out.append("/boot=");
    append_u64(out, authority.publisher_boot.value());
    out.append(" valid=[");
    append_u64(out, authority.validity.issued_at);
    out.push_back(',');
    append_u64(out, authority.validity.expires_at);
    out.append(") bind=");
    append_u64(out, authority.bound_flags);
    return out;
}

namespace {

void add_line(std::vector<ExplanationLine>& out, bool& truncated, ExplanationKind kind,
              const std::string& text) {
    if (out.size() >= limits::kMaxExplanationLines) {
        truncated = true;
        return;
    }
    out.push_back(ExplanationLine{kind, text});
}

ExplanationKind classify_reason(std::string_view name) {
    if (name.rfind("below_", 0) == 0 || name == "policy_rule_not_satisfied") {
        return ExplanationKind::Disqualification;
    }
    if (name == "evidence_stale" || name == "telemetry_gap" || name == "revalidation_required" ||
        name == "evidence_missing" || name == "evidence_unknown_field" ||
        name == "capacity_missing" || name == "capacity_stale" || name == "path_unknown" ||
        name == "path_generation_changed" || name == "flow_generation_changed" ||
        name == "policy_changed" || name == "flow_completed" ||
        name == "policy_rule_indeterminate") {
        return ExplanationKind::Suspension;
    }
    if (name == "protected_obligation" || name == "non_preemptible_obligation" ||
        name == "reserved_capacity_bounded" || name == "control_class_bounded") {
        return ExplanationKind::Protection;
    }
    if (name == "governance_authorized" || name == "governance_suppressed" ||
        name == "governance_clamped_to_bounds") {
        return ExplanationKind::Authority;
    }
    if (name == "impact_below_action_threshold" || name == "contention_below_action_threshold") {
        return ExplanationKind::Suppression;
    }
    return ExplanationKind::Qualification;
}

}  // namespace

std::vector<ExplanationLine> build_explanation_lines(const Explanation& explanation,
                                                      bool& truncated) {
    std::vector<ExplanationLine> lines;
    truncated = explanation.truncated;
    if (!explanation.found) {
        add_line(lines, truncated, ExplanationKind::Indeterminate,
                 "flow generation is not tracked by this governor");
        return lines;
    }

    {
        std::string text{"state="};
        text.append(to_string(explanation.state));
        text.append(" authority=");
        text.append(to_string(explanation.authority));
        add_line(lines, truncated, ExplanationKind::Qualification, text);
    }

    for (const std::string_view name : reason_names(explanation.reasons)) {
        add_line(lines, truncated, classify_reason(name), std::string{name});
    }

    const FlowMeasurements& m = explanation.measurements;
    {
        std::string text{"observed=["};
        append_u64(text, m.observed.begin);
        text.push_back(',');
        append_u64(text, m.observed.end);
        text.append(") duration_ticks=");
        append_u64(text, m.duration_ticks);
        text.append(" cumulative_bytes=");
        append_u64(text, m.cumulative_bytes);
        text.append(" sustained_rate_per_kilotick=");
        append_u64(text, m.sustained_rate);
        text.append(" peak_window_rate=");
        append_u64(text, m.peak_window_rate);
        text.append(" windows=");
        append_u64(text, m.window_count);
        text.append(" dropped=");
        append_u64(text, m.dropped_windows);
        add_line(lines, truncated, ExplanationKind::Qualification, text);
    }
    {
        std::string text{"service_class="};
        text.append(to_string(m.service_class));
        text.append(" priority=");
        if (m.priority.known()) {
            append_u64(text, m.priority.level());
        } else {
            text.append("unknown");
        }
        text.append(" protection=");
        text.append(to_string(m.protection));
        text.append(" reservation=");
        append_u64(text, m.reservation.value());
        text.append(" contiguous=");
        text.append(m.evidence_contiguous ? "true" : "false");
        text.append(" gap=");
        text.append(m.gap_detected ? "true" : "false");
        add_line(lines, truncated, ExplanationKind::Qualification, text);
    }

    if (m.capacity_known) {
        std::string text{"binding_resource="};
        append_u64(text, m.binding_resource.value());
        text.append(" capacity_per_kilotick=");
        append_u64(text, m.resource_capacity);
        text.append(" reserved=");
        append_u64(text, m.resource_reserved);
        text.append(" available=");
        append_u64(text, m.resource_available);
        text.append(" share_of_capacity=");
        text.append(to_percent(m.share_of_capacity));
        text.append(" share_of_available=");
        text.append(to_percent(m.share_of_available));
        add_line(lines, truncated, ExplanationKind::Impact, text);
    } else {
        add_line(lines, truncated, ExplanationKind::Impact,
                 "binding_resource=unknown share=unknown");
    }

    for (const ThresholdTraceEntry& entry : explanation.trace) {
        std::string text{"threshold "};
        text.append(to_string(entry.kind));
        text.append(" node=");
        append_u64(text, entry.node_index);
        text.append(" threshold=");
        append_u64(text, entry.threshold_value);
        text.append(" observed=");
        if (entry.observed_defined) {
            append_u64(text, entry.observed_value);
        } else {
            text.append("undefined");
        }
        text.append(" result=");
        text.append(to_string(entry.result));
        add_line(lines, truncated, ExplanationKind::Qualification, text);
    }

    {
        const ImpactAssessment& impact = explanation.impact;
        std::string text{"severity="};
        text.append(to_percent(impact.severity));
        text.append(" contention_pressure=");
        text.append(to_percent(impact.contention_pressure));
        text.append(" rate_pressure=");
        text.append(to_percent(impact.rate_pressure));
        text.append(" competing_flows=");
        append_u64(text, impact.competing_flows);
        text.append(" competing_elephants=");
        append_u64(text, impact.competing_elephants);
        text.append(" excess_rate=");
        append_u64(text, impact.excess_rate);
        text.append(" excess_over_guarantee=");
        append_u64(text, impact.excess_over_guarantee);
        text.append(" protected_conflict=");
        text.append(impact.protected_obligation_conflict ? "true" : "false");
        add_line(lines, truncated, ExplanationKind::Impact, text);
    }

    add_line(lines, truncated, ExplanationKind::Authority,
             render_authority_vector(explanation.authority_vector));

    for (const GovernanceIntent& intent : explanation.intents) {
        std::string text{"intent "};
        text.append(to_string(intent.kind));
        text.append(" state=");
        text.append(to_string(intent.state));
        text.append(" suppressed=");
        text.append(to_string(intent.suppressed));
        text.append(" granted_rate_reduction=");
        text.append(to_percent(intent.granted.rate_reduction_bp));
        text.append(" window=[");
        append_u64(text, intent.window.begin);
        text.push_back(',');
        append_u64(text, intent.window.end);
        text.push_back(')');
        add_line(lines, truncated, ExplanationKind::Intent, text);
    }

    {
        std::string text{"history_digest="};
        append_u64(text, explanation.history_digest);
        text.append(" folded=");
        append_u64(text, explanation.history_folded);
        add_line(lines, truncated, ExplanationKind::History, text);
    }
    return lines;
}

std::string render_text(const Explanation& explanation) {
    bool truncated = false;
    const std::vector<ExplanationLine> lines = build_explanation_lines(explanation, truncated);
    std::string out;
    out.reserve(1024);
    out.append("flow ");
    append_u64(out, explanation.flow.value());
    out.append(" generation ");
    append_u64(out, explanation.flow_generation.value());
    out.push_back(kNewline);
    for (const ExplanationLine& line : lines) {
        out.append("  [");
        out.append(to_string(line.kind));
        out.append("] ");
        out.append(line.text);
        out.push_back(kNewline);
        if (out.size() >= limits::kMaxExplanationText) {
            truncated = true;
            break;
        }
    }
    if (truncated) {
        out.append("  [history] additional lines omitted at the explanation ceiling");
        out.push_back(kNewline);
    }
    out.append("explanation_digest=");
    append_u64(out, explanation.digest());
    out.push_back(kNewline);
    return out;
}

std::string render_json(const Explanation& explanation) {
    bool truncated = false;
    const std::vector<ExplanationLine> lines = build_explanation_lines(explanation, truncated);
    const FlowMeasurements& m = explanation.measurements;
    const ImpactAssessment& impact = explanation.impact;

    std::string out;
    out.reserve(2048);
    out.append("{");
    out.push_back(kNewline);
    out.append("  \"found\": ");
    out.append(explanation.found ? "true" : "false");
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"flow\": ");
    append_u64(out, explanation.flow.value());
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"flow_generation\": ");
    append_u64(out, explanation.flow_generation.value());
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"state\": ");
    append_json_string(out, to_string(explanation.state));
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"authority\": ");
    append_json_string(out, to_string(explanation.authority));
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"reasons\": [");
    {
        bool first = true;
        for (const std::string_view name : reason_names(explanation.reasons)) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            append_json_string(out, name);
        }
    }
    out.append("],");
    out.push_back(kNewline);
    out.append("  \"measurements\": {");
    out.push_back(kNewline);
    out.append("    \"duration_ticks\": ");
    append_u64(out, m.duration_ticks);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"cumulative_bytes\": ");
    append_u64(out, m.cumulative_bytes);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"sustained_rate_per_kilotick\": ");
    append_u64(out, m.sustained_rate);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"peak_window_rate_per_kilotick\": ");
    append_u64(out, m.peak_window_rate);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"window_count\": ");
    append_u64(out, m.window_count);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"dropped_windows\": ");
    append_u64(out, m.dropped_windows);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"evidence_present\": ");
    out.append(m.evidence_present ? "true" : "false");
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"evidence_contiguous\": ");
    out.append(m.evidence_contiguous ? "true" : "false");
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"gap_detected\": ");
    out.append(m.gap_detected ? "true" : "false");
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"service_class\": ");
    append_json_string(out, to_string(m.service_class));
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"priority_known\": ");
    out.append(m.priority.known() ? "true" : "false");
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"priority\": ");
    append_u64(out, m.priority.level());
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"protection\": ");
    append_json_string(out, to_string(m.protection));
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"reservation\": ");
    append_u64(out, m.reservation.value());
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"capacity_known\": ");
    out.append(m.capacity_known ? "true" : "false");
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"binding_resource\": ");
    append_u64(out, m.binding_resource.value());
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"resource_capacity_per_kilotick\": ");
    append_u64(out, m.resource_capacity);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"resource_reserved_per_kilotick\": ");
    append_u64(out, m.resource_reserved);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"resource_available_per_kilotick\": ");
    append_u64(out, m.resource_available);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"share_of_capacity_bp\": ");
    append_u64(out, m.share_of_capacity);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"share_of_available_bp\": ");
    append_u64(out, m.share_of_available);
    out.push_back(kNewline);
    out.append("  },");
    out.push_back(kNewline);
    out.append("  \"thresholds\": [");
    {
        bool first = true;
        for (const ThresholdTraceEntry& entry : explanation.trace) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            out.push_back(kNewline);
            out.append("    {\"node\": ");
            append_u64(out, entry.node_index);
            out.append(", \"kind\": ");
            append_json_string(out, to_string(entry.kind));
            out.append(", \"threshold\": ");
            append_u64(out, entry.threshold_value);
            out.append(", \"observed_defined\": ");
            out.append(entry.observed_defined ? "true" : "false");
            out.append(", \"observed\": ");
            append_u64(out, entry.observed_value);
            out.append(", \"result\": ");
            append_json_string(out, to_string(entry.result));
            out.push_back('}');
        }
        if (!explanation.trace.empty()) {
            out.push_back(kNewline);
            out.append("  ");
        }
    }
    out.append("],");
    out.push_back(kNewline);
    out.append("  \"impact\": {");
    out.push_back(kNewline);
    out.append("    \"severity_bp\": ");
    append_u64(out, impact.severity);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"contention_pressure_bp\": ");
    append_u64(out, impact.contention_pressure);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"rate_pressure_bp\": ");
    append_u64(out, impact.rate_pressure);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"competing_flows\": ");
    append_u64(out, impact.competing_flows);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"competing_elephants\": ");
    append_u64(out, impact.competing_elephants);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"excess_rate\": ");
    append_u64(out, impact.excess_rate);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"excess_over_guarantee\": ");
    append_u64(out, impact.excess_over_guarantee);
    out.append(",");
    out.push_back(kNewline);
    out.append("    \"protected_obligation_conflict\": ");
    out.append(impact.protected_obligation_conflict ? "true" : "false");
    out.push_back(kNewline);
    out.append("  },");
    out.push_back(kNewline);
    out.append("  \"authority_vector\": ");
    append_json_string(out, render_authority_vector(explanation.authority_vector));
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"history_digest\": ");
    append_u64(out, explanation.history_digest);
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"history_folded\": ");
    append_u64(out, explanation.history_folded);
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"intents\": [");
    {
        bool first = true;
        for (const GovernanceIntent& intent : explanation.intents) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            out.push_back(kNewline);
            out.append("    {\"kind\": ");
            append_json_string(out, to_string(intent.kind));
            out.append(", \"state\": ");
            append_json_string(out, to_string(intent.state));
            out.append(", \"suppressed\": ");
            append_json_string(out, to_string(intent.suppressed));
            out.append(", \"rate_reduction_bp\": ");
            append_u64(out, intent.granted.rate_reduction_bp);
            out.append(", \"targets\": ");
            append_u64(out, intent.granted.targets);
            out.append(", \"window_begin\": ");
            append_u64(out, intent.window.begin);
            out.append(", \"window_end\": ");
            append_u64(out, intent.window.end);
            out.push_back('}');
        }
        if (!explanation.intents.empty()) {
            out.push_back(kNewline);
            out.append("  ");
        }
    }
    out.append("],");
    out.push_back(kNewline);
    out.append("  \"lines\": [");
    {
        bool first = true;
        for (const ExplanationLine& line : lines) {
            if (!first) {
                out.push_back(',');
            }
            first = false;
            out.push_back(kNewline);
            out.append("    {\"kind\": ");
            append_json_string(out, to_string(line.kind));
            out.append(", \"text\": ");
            append_json_string(out, line.text);
            out.push_back('}');
        }
        if (!lines.empty()) {
            out.push_back(kNewline);
            out.append("  ");
        }
    }
    out.append("],");
    out.push_back(kNewline);
    out.append("  \"truncated\": ");
    out.append(truncated ? "true" : "false");
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"explanation_digest\": ");
    append_u64(out, explanation.digest());
    out.push_back(kNewline);
    out.append("}");
    out.push_back(kNewline);
    return out;
}

}  // namespace efg
