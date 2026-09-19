// Elephant Flow Governor - policy.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A policy is a bounded, flattened, acyclic rule tree over FlowMeasurements plus
// the authorization envelope that bounds what governance intent may be produced.
// Policy evaluation is pure: the same measurements and the same policy document
// always produce the same three valued result and the same trace.

#ifndef EFG_POLICY_HPP
#define EFG_POLICY_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "efg/checked.hpp"
#include "efg/flow.hpp"
#include "efg/hash.hpp"
#include "efg/identity.hpp"
#include "efg/intent_kind.hpp"
#include "efg/limits.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"
#include "efg/tri.hpp"

namespace efg {

enum class ThresholdKind : std::uint8_t {
    Always = 0,
    Never = 1,
    CumulativeBytes = 2,
    DurationTicks = 3,
    SustainedRate = 4,
    PeakRate = 5,
    ShareOfCapacity = 6,
    ShareOfAvailable = 7,
    CompetingFlows = 8,
    CompetingElephants = 9,
    ExcessOverGuarantee = 10,
    ServiceClassIs = 11,
    PriorityAtLeast = 12,
    IsProtected = 13,
    IsNonPreemptible = 14,
    HasReservation = 15,

    // Sentinel used only for validation.
    Count = 16,
};

[[nodiscard]] std::string_view to_string(ThresholdKind kind) noexcept;
[[nodiscard]] bool parse_threshold_kind(std::string_view text, ThresholdKind& out) noexcept;

struct Threshold {
    ThresholdKind kind{ThresholdKind::Always};
    u64 value{0};
    ServiceClass class_operand{ServiceClass::Unknown};

    /// Evaluate against measurements. Returns Unknown when a required input is
    /// absent, so that missing evidence can never satisfy a rule.
    [[nodiscard]] Tri evaluate(const FlowMeasurements& measurements) const;

    /// The observed quantity this threshold compares against, when defined.
    [[nodiscard]] bool observed(const FlowMeasurements& measurements, u64& out) const;
};

enum class RuleKind : std::uint8_t {
    Threshold = 0,
    All = 1,
    Any = 2,
    Not = 3,
};

[[nodiscard]] std::string_view to_string(RuleKind kind) noexcept;

struct RuleNode {
    RuleKind kind{RuleKind::All};
    Threshold threshold{};
    /// Child node indices. Bounded by limits::kMaxPolicyNodes.
    std::vector<u32> children{};
};

/// One evaluated threshold, retained for explanation. Bounded by construction.
struct ThresholdTraceEntry {
    u32 node_index{0};
    ThresholdKind kind{ThresholdKind::Always};
    Tri result{Tri::Unknown};
    u64 observed_value{0};
    u64 threshold_value{0};
    bool observed_defined{false};
};

struct ThresholdTrace {
    std::vector<ThresholdTraceEntry> entries{};
    bool truncated{false};

    void record(u32 node_index, const Threshold& threshold, Tri result,
                const FlowMeasurements& measurements);
    void clear() noexcept {
        entries.clear();
        truncated = false;
    }
};

struct RuleSet {
    std::vector<RuleNode> nodes{};
    u32 root{0};

    [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }

    /// Structural validation: bounded size, indices in range, acyclic, bounded
    /// depth, arity consistent with the node kind.
    [[nodiscard]] Status validate() const;

    [[nodiscard]] Tri evaluate(const FlowMeasurements& measurements,
                               ThresholdTrace* trace = nullptr) const;

    [[nodiscard]] u64 digest() const;
};

void encode(ByteWriter& writer, const RuleSet& rules);

/// Anti-flapping configuration. Entering the elephant state and leaving it are
/// separate decisions with separate confirmation requirements.
struct HysteresisSpec {
    u32 enter_confirm_windows{1};
    u32 exit_confirm_windows{3};

    /// Used to synthesize the exit rule from the enter rule when no explicit
    /// exit rule is supplied. 8000 means the flow must fall to 80% of the enter
    /// thresholds before the exit rule can fire.
    BasisPoints exit_scale_bp{8000};

    [[nodiscard]] Status validate() const;
};

/// The authorization envelope. Intent produced by the governor is clamped to
/// these bounds; a request that exceeds them is either clamped (and labelled) or
/// suppressed, never silently widened.
struct IntentAuthorization {
    bool allow_alternate_placement{true};
    bool allow_rate_shaping{true};
    bool allow_scheduling_isolation{true};
    bool allow_competing_admission_reduction{true};
    bool allow_congestion_escalation{true};

    /// When true, no intent may act against a protected, non-preemptible,
    /// reserved or control-class flow, and reserved capacity is never targeted.
    bool protect_obligations{true};

    /// Maximum portion of a flow's sustained rate that rate shaping may request.
    BasisPoints max_shaping_reduction_bp{2500};

    /// Maximum lifetime of a single governance intent.
    u64 max_intent_duration_ticks{60000};

    /// Maximum number of targets a single intent may name.
    u32 max_targets{8};

    /// Minimum composite severity before any corrective intent is produced.
    BasisPoints min_impact_severity{1000};

    /// Minimum contention pressure before placement or admission intent is produced.
    BasisPoints min_contention_pressure{500};

    [[nodiscard]] Status validate() const;

    /// True when the envelope permits the supplied intent kind.
    [[nodiscard]] bool permits(IntentKind kind) const noexcept;
};

/// Complete, versioned policy document.
struct PolicyDocument {
    PolicyId id{};
    Generation generation{};
    RuleSet enter_rule{};
    RuleSet exit_rule{};
    HysteresisSpec hysteresis{};
    IntentAuthorization authorization{};
    ValidityWindow validity{};
    Provenance provenance{};
    u64 digest{0};

    [[nodiscard]] Status validate() const;

    /// Compute and store the canonical digest. Must be called before submission.
    Status finalize();

    [[nodiscard]] Tri evaluate_enter(const FlowMeasurements& measurements,
                                     ThresholdTrace* trace = nullptr) const {
        return enter_rule.evaluate(measurements, trace);
    }

    /// Exit evaluation. When no explicit exit rule is present the enter rule is
    /// re-evaluated against scaled thresholds, which keeps the exit condition
    /// strictly weaker than the enter condition.
    [[nodiscard]] Tri evaluate_exit(const FlowMeasurements& measurements,
                                    ThresholdTrace* trace = nullptr) const;
};

void encode(ByteWriter& writer, const PolicyDocument& policy);

/// The exit rule that actually governs hysteresis: the explicit exit rule when
/// the policy supplies one, otherwise the enter rule scaled down by the exit
/// scale. Callers cache this rather than recomputing it per evaluation.
[[nodiscard]] StatusOr<RuleSet> effective_exit_rule(const PolicyDocument& policy);

/// Scaled copy of a rule set used to synthesize an exit rule. Values are scaled
/// down by the supplied basis points; class and boolean thresholds are preserved
/// and only their direction is inverted where meaningful.
[[nodiscard]] StatusOr<RuleSet> scale_rule_set(const RuleSet& rules, BasisPoints scale_bp);

[[nodiscard]] std::string_view describe_threshold(const Threshold& threshold);

}  // namespace efg

#endif  // EFG_POLICY_HPP
