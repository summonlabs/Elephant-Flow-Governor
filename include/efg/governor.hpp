// Elephant Flow Governor - the governor.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The governor owns exactly one thing: deciding which flows qualify as elephants
// now, what impact they create, which bounded governance intent is authorized,
// and when that authority must be revoked, fenced or revalidated.
//
// It owns no forwarding, no placement, no scheduling, no shaping, no admission
// control, no reservation and no congestion synthesis. Every output of this class
// is either a classification or an intent; nothing here executes.

#ifndef EFG_GOVERNOR_HPP
#define EFG_GOVERNOR_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "efg/capacity.hpp"
#include "efg/checked.hpp"
#include "efg/classification.hpp"
#include "efg/explain.hpp"
#include "efg/flow.hpp"
#include "efg/hash.hpp"
#include "efg/identity.hpp"
#include "efg/intervention.hpp"
#include "efg/limits.hpp"
#include "efg/path.hpp"
#include "efg/policy.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"

namespace efg {

struct GovernorConfig {
    /// Maximum number of distinct (flow, generation) records retained at once.
    std::size_t max_flows{32768};
    /// Evidence windows retained per flow generation. Clamped to
    /// [2, limits::kMaxSamplesPerFlow].
    std::size_t samples_per_flow{8};
    /// Retained classification transitions per flow generation.
    std::size_t max_history_per_flow{limits::kMaxHistoryPerFlow};
    /// Retained threshold evaluations per decision.
    std::size_t max_trace_entries{limits::kMaxTraceEntries};
    /// Lifetime of a classification once decided.
    u64 classification_validity_ticks{1000};
    /// A gap larger than this between consecutive evidence windows breaks the
    /// evidence stream and forces revalidation.
    u64 max_telemetry_gap_ticks{100};
    /// Default lifetime of a governance intent when the policy does not narrow it.
    u64 intent_duration_ticks{500};
    /// Ceiling on cumulative bytes per flow generation. 0 disables the ceiling.
    u64 max_cumulative_bytes{0};
    /// Ceiling on bytes reported inside a single evidence window. 0 disables.
    u64 max_bytes_per_window{0};
    /// Ceiling on the population of governance intents retained.
    std::size_t max_intents{limits::kMaxIntents};

    [[nodiscard]] Status validate() const;
};

struct GovernorCounters {
    u64 evidence_accepted{0};
    u64 evidence_duplicates{0};
    u64 evidence_rejected{0};
    u64 evidence_contradictory{0};
    u64 evidence_gaps{0};
    u64 evidence_after_completion{0};

    u64 capacity_accepted{0};
    u64 capacity_rejected{0};
    u64 path_accepted{0};
    u64 path_rejected{0};
    u64 policy_accepted{0};
    u64 policy_rejected{0};

    u64 decisions{0};
    u64 elephant_decisions{0};
    u64 not_elephant_decisions{0};
    u64 unknown_decisions{0};
    u64 suspended_decisions{0};
    u64 transitions{0};
    u64 revalidations{0};
    u64 authority_revocations{0};

    u64 flow_generation_rollovers{0};
    u64 flow_completions{0};
    u64 flow_evictions{0};
    u64 capacity_refusals{0};
    u64 stale_refusals{0};

    u64 ticks{0};
    u64 sweeps{0};
    u64 intents_proposed{0};
    u64 intents_authorized{0};
    u64 intents_suppressed{0};
    u64 intents_clamped{0};
    u64 intents_expired{0};
    u64 intents_revoked{0};
    u64 intents_fenced{0};
    u64 classifications_expired{0};

    u64 restore_events{0};
    u64 restored_flows{0};
    u64 restored_flows_requiring_revalidation{0};
};

/// Result of evaluating one flow generation.
struct FlowDecision {
    FlowClassification classification{};
    std::vector<GovernanceIntent> intents{};
    bool state_changed{false};
    bool authority_changed{false};
    bool evidence_duplicate{false};
};

/// Result of a deterministic sweep at a logical tick. Only flows whose state or
/// authority changed appear in the batch, so the batch is bounded by the number
/// of real transitions rather than by the population size.
struct DecisionBatch {
    Tick now{0};
    std::vector<FlowDecision> decisions{};
    u64 suspensions{0};
    u64 revalidations{0};
    u64 revocations{0};
    u64 fences{0};
    u64 expirations{0};
    u64 completions{0};
};

/// The externally visible identity of this governor incarnation. Restoring
/// durable state never restores the boot identity: a restart mints a new boot
/// identifier and advances the epoch, which fences every intent produced by the
/// previous incarnation.
struct GovernorIncarnation {
    BootIdentity boot{};
    u64 minted_from{0};
};

/// Compose final measurements for a flow from its ledger plus path, capacity and
/// population context. Pure function.
[[nodiscard]] StatusOr<FlowMeasurements> compose_measurements(
    const FlowLedger& ledger,
    const PathDescriptor* path,
    const CapacityTable& capacities,
    u32 competing_flows,
    u32 competing_elephants,
    bool require_capacity);

class Governor {
public:
    explicit Governor(GovernorConfig config = {});
    ~Governor();

    Governor(const Governor&) = delete;
    Governor& operator=(const Governor&) = delete;
    Governor(Governor&&) = delete;
    Governor& operator=(Governor&&) = delete;

    // --- Incarnation -------------------------------------------------------
    void set_incarnation(BootIdentity boot) noexcept { incarnation_.boot = boot; }
    [[nodiscard]] const BootIdentity& boot() const noexcept { return incarnation_.boot; }
    [[nodiscard]] Tick last_tick() const noexcept { return last_tick_; }
    [[nodiscard]] const GovernorConfig& config() const noexcept { return config_; }

    // --- Authoritative inputs ---------------------------------------------
    Status submit_policy(PolicyDocument policy);
    Status submit_capacity(ResourceCapacity snapshot);
    Status submit_path(PathDescriptor descriptor);
    [[nodiscard]] StatusOr<FlowDecision> submit_evidence(const FlowSample& sample);
    Status complete_flow(FlowId flow, Generation generation, Tick at);

    // --- Deterministic sweeps ---------------------------------------------
    [[nodiscard]] StatusOr<DecisionBatch> tick(Tick now);

    // --- Queries -----------------------------------------------------------
    /// Re-evaluate the flow generation at the supplied tick and return the
    /// decision. This is a live answer, not a cached one: it observes the same
    /// invariants as submit_evidence and may suspend or re-qualify the flow.
    [[nodiscard]] StatusOr<FlowClassification> classification(FlowId flow, Generation generation,
                                                             Tick now);
    [[nodiscard]] StatusOr<Explanation> explain(FlowId flow, Generation generation, Tick now);

    [[nodiscard]] const IntentLedger& intents() const noexcept { return intents_; }
    [[nodiscard]] IntentLedger& intents() noexcept { return intents_; }
    [[nodiscard]] const PolicyDocument* policy() const noexcept {
        return policy_.has_value() ? &policy_.value() : nullptr;
    }
    [[nodiscard]] const CapacityTable& capacities() const noexcept { return capacities_; }
    [[nodiscard]] const PathTable& paths() const noexcept { return paths_; }
    [[nodiscard]] const GovernorCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] std::size_t flow_count() const noexcept { return flows_.size(); }
    [[nodiscard]] std::vector<FlowId> flow_ids() const;
    /// Every tracked (flow, generation) pair in ascending key order.
    [[nodiscard]] std::vector<std::pair<FlowId, Generation>> flow_keys() const;
    [[nodiscard]] const FlowLedger* ledger(FlowId flow, Generation generation) const;
    [[nodiscard]] const ClassificationHistory* history(FlowId flow, Generation generation) const;

    // --- Durable state -----------------------------------------------------
    /// Serialize committed state. Returns false through Status when the state
    /// would exceed the supplied ceiling.
    [[nodiscard]] Status serialize(ByteWriter& writer) const;
    /// Restore committed state at an explicit tick. Restored classifications are
    /// historical: they are re-marked Suspended, their authority is dropped and
    /// live intents are fenced. Nothing about liveness, telemetry freshness or
    /// publisher authority is resurrected.
    Status deserialize(ByteReader& reader, Tick now);

    /// Drop every retained record and return accounting to a pristine baseline.
    void reset() noexcept;

private:
    struct FlowRecord {
        FlowLedger ledger{};
        ClassificationHistory history{};
        FlowClassification classification{};
        u32 enter_streak{0};
        u32 exit_streak{0};
        bool has_classification{false};
        Tick evaluated_tick{0};
        EvidenceWindowId evaluated_window{};
        ResourceId bound_resource{};
        BasisPoints bound_share{0};
        bool bound_elephant{false};
        bool bound{false};
        Tick completed_tick{0};
    };

    using FlowKey = std::pair<FlowId, Generation>;

    struct BindingEntry {
        bool elephant{false};
        BasisPoints share{0};
    };

    struct ResourceAggregate {
        u32 flows{0};
        u32 elephants{0};
        u64 elephant_share_sum{0};
    };

    [[nodiscard]] Status ensure_flow_capacity(FlowId flow);
    [[nodiscard]] FlowRecord& create_flow(FlowId flow, Generation generation, bool& created);
    [[nodiscard]] StatusOr<FlowDecision> evaluate(FlowRecord& record, Tick now);
    [[nodiscard]] StatusOr<AuthorityVector> build_authority(const FlowRecord& record, Tick now) const;
    Status enforce_protection(FlowClassification& cls) const;
    [[nodiscard]] Status produce_intents(FlowRecord& record,
                                         std::vector<GovernanceIntent>& out, Tick now);
    [[nodiscard]] StatusOr<IntentBounds> ceiling_bounds(Tick now) const;
    [[nodiscard]] ReasonCode suspension_reasons(const FlowRecord& record, Tick now,
                                                bool& suspend) const;
    void update_index(const FlowKey& key, FlowRecord& record);
    Status invalidate_all_authority(Tick now, ReasonCode reason);
    Status resume_after_restore(Tick now);
    [[nodiscard]] bool evict_one_completed_flow();

    GovernorConfig config_{};
    GovernorIncarnation incarnation_{};
    TickGuard tick_guard_{};
    Tick last_tick_{0};

    std::optional<PolicyDocument> policy_{};
    RuleSet exit_rules_{};
    CapacityTable capacities_{};
    PathTable paths_{};
    std::map<FlowKey, FlowRecord> flows_{};
    std::map<FlowId, Generation> highest_generation_{};
    std::map<ResourceId, std::map<FlowKey, BindingEntry>> binding_index_{};
    std::map<ResourceId, ResourceAggregate> resource_aggregate_{};
    IntentLedger intents_{};
    u64 classification_sequence_{0};
    u64 intent_sequence_{0};
    GovernorCounters counters_{};
};

/// Canonical digest of the entire committed decision surface. Two governors fed
/// the same inputs in the same order produce the same digest; this is the
/// determinism proof used by the property tests.
[[nodiscard]] StatusOr<u64> decision_digest(const Governor& governor);

}  // namespace efg

#endif  // EFG_GOVERNOR_HPP
