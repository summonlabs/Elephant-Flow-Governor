// Elephant Flow Governor - classification.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A classification is a statement about one flow generation, justified by named
// evidence windows, one capacity snapshot generation, one path generation and one
// policy generation, valid until a tick. It is not a punishment: a protected
// high-volume flow classifies as an elephant and is still never throttled.

#ifndef EFG_CLASSIFICATION_HPP
#define EFG_CLASSIFICATION_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "efg/checked.hpp"
#include "efg/flow.hpp"
#include "efg/hash.hpp"
#include "efg/identity.hpp"
#include "efg/impact.hpp"
#include "efg/limits.hpp"
#include "efg/policy.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"
#include "efg/tri.hpp"

namespace efg {

enum class ElephantState : std::uint8_t {
    /// No usable evidence, or evidence whose required fields are Unknown.
    Unknown = 0,
    /// Evidence is authoritative and the enter rule is definitively false.
    NotElephant = 1,
    /// The flow qualifies as an elephant now.
    Elephant = 2,
    /// Previously classified, but the justification no longer holds: evidence
    /// aged out, a telemetry gap opened, a path or policy generation changed, or
    /// the publisher incarnation was replaced. Authority is revoked and must be
    /// re-earned from fresh evidence.
    Suspended = 3,
};

[[nodiscard]] std::string_view to_string(ElephantState state) noexcept;

/// How much the current classification may authorize.
enum class AuthorityLevel : std::uint8_t {
    /// No authority at all. Unknown and Suspended always map here.
    None = 0,
    /// The classification is authoritative and may be reported, but no
    /// corrective intent may be produced from it.
    Observe = 1,
    /// The classification may authorize bounded corrective intent.
    Govern = 2,
};

[[nodiscard]] std::string_view to_string(AuthorityLevel level) noexcept;

/// Flags describing which identity tokens a classification was bound to.
enum : std::uint32_t {
    kAuthorityFlowBound = 1u << 0,
    kAuthorityPathBound = 1u << 1,
    kAuthorityCapacityBound = 1u << 2,
    kAuthorityPolicyBound = 1u << 3,
    kAuthorityEpochBound = 1u << 4,
    kAuthorityPublisherBound = 1u << 5,
    kAuthorityWindowBound = 1u << 6,
    kAuthorityBootBound = 1u << 7,
};

/// The exact identities and generations that justified one decision. Authority
/// is valid only while every bound token still matches.
struct AuthorityVector {
    FlowId flow{};
    Generation flow_generation{};
    PathId path{};
    Generation path_generation{};
    CapacitySnapshotId capacity{};
    Generation capacity_generation{};
    EvidenceWindowId window{};
    SequenceNumber window_sequence{0};
    PolicyId policy{};
    Generation policy_generation{};
    Generation classification_generation{};
    EpochId epoch{};
    BootId boot{};
    PublisherId publisher{};
    BootId publisher_boot{};
    ValidityWindow validity{};
    std::uint32_t bound_flags{0};

    [[nodiscard]] bool binds(std::uint32_t flag) const noexcept {
        return (bound_flags & flag) != 0;
    }

    /// Compare against a currently observed authority vector. Returns Match when
    /// every flag bound in *this* still agrees; otherwise names the first
    /// divergence in a fixed, deterministic order.
    [[nodiscard]] BindingVerdict compare(const AuthorityVector& current) const;

    [[nodiscard]] u64 digest() const;
};

void encode(ByteWriter& writer, const AuthorityVector& authority);

/// One classification decision.
struct FlowClassification {
    ClassificationId id{};
    Generation generation{};
    FlowId flow{};
    Generation flow_generation{};
    ElephantState state{ElephantState::Unknown};
    AuthorityLevel authority{AuthorityLevel::None};
    AuthorityVector authority_vector{};
    ReasonCode reasons{ReasonCode::None};
    Tick decided_at{0};
    ValidityWindow validity{};
    FlowMeasurements measurements{};
    ImpactAssessment impact{};
    std::vector<ThresholdTraceEntry> trace{};
    bool trace_truncated{false};
    u32 enter_streak{0};
    u32 exit_streak{0};
    u64 history_digest{0};
    u64 digest{0};

    /// True when the classification may authorize bounded corrective intent.
    [[nodiscard]] bool authorizes_governance() const noexcept {
        return state == ElephantState::Elephant && authority == AuthorityLevel::Govern;
    }

    /// True when the flow carries a protection obligation.
    [[nodiscard]] bool is_protected() const noexcept {
        return measurements.protection == ProtectionState::Protected ||
               measurements.protection == ProtectionState::NonPreemptible ||
               measurements.has_reservation ||
               measurements.service_class == ServiceClass::Reserved ||
               measurements.service_class == ServiceClass::Control;
    }

    [[nodiscard]] u64 compute_digest() const;
};

void encode(ByteWriter& writer, const FlowClassification& classification);

/// A single recorded transition in a flow generation's classification history.
struct ClassificationTransition {
    Tick at{0};
    ElephantState from{ElephantState::Unknown};
    ElephantState to{ElephantState::Unknown};
    AuthorityLevel authority{AuthorityLevel::None};
    ReasonCode reasons{ReasonCode::None};
    ClassificationId classification{};
    Generation classification_generation{};
};

void encode(ByteWriter& writer, const ClassificationTransition& transition);

/// Append-only, bounded, deterministic history for one flow generation.
///
/// Determinism is provable: the history digest is a function of the ordered
/// sequence of transitions. Replaying the same evidence in the same order
/// reproduces the digest exactly. When the retention bound is reached the oldest
/// transitions are folded into a rolling digest rather than dropped silently, so
/// durable growth is bounded without losing verifiability.
class ClassificationHistory {
public:
    explicit ClassificationHistory(std::size_t max_entries = limits::kMaxHistoryPerFlow)
        : max_entries_(max_entries == 0 ? 1 : max_entries) {}

    Status record(const ClassificationTransition& transition);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] u64 folded_count() const noexcept { return folded_count_; }
    [[nodiscard]] u64 digest() const noexcept { return digest_; }
    [[nodiscard]] const ClassificationTransition* latest() const noexcept {
        return entries_.empty() ? nullptr : &entries_.back();
    }
    [[nodiscard]] std::vector<ClassificationTransition> transitions() const;

    void clear() noexcept;

private:
    std::vector<ClassificationTransition> entries_;
    std::size_t max_entries_;
    u64 digest_{0};
    u64 folded_count_{0};
};

}  // namespace efg

#endif  // EFG_CLASSIFICATION_HPP
