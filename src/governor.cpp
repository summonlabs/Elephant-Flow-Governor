// Elephant Flow Governor - governor implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/governor.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "efg/codec.hpp"

namespace efg {

Status GovernorConfig::validate() const {
    if (max_flows == 0 || max_flows > limits::kMaxFlows) {
        return make_status(StatusCode::OutOfRange, "max_flows is outside the supported range");
    }
    if (samples_per_flow < 2 || samples_per_flow > limits::kMaxSamplesPerFlow) {
        return make_status(StatusCode::OutOfRange, "samples_per_flow is outside the supported range");
    }
    if (max_history_per_flow == 0 || max_history_per_flow > limits::kMaxHistoryPerFlow) {
        return make_status(StatusCode::OutOfRange,
                           "max_history_per_flow is outside the supported range");
    }
    if (max_trace_entries == 0 || max_trace_entries > limits::kMaxTraceEntries) {
        return make_status(StatusCode::OutOfRange, "max_trace_entries is outside the supported range");
    }
    if (classification_validity_ticks == 0) {
        return make_status(StatusCode::InvalidArgument, "classification validity must be positive");
    }
    if (intent_duration_ticks == 0) {
        return make_status(StatusCode::InvalidArgument, "intent lifetime must be positive");
    }
    if (max_intents == 0 || max_intents > limits::kMaxIntents) {
        return make_status(StatusCode::OutOfRange, "max_intents is outside the supported range");
    }
    return {};
}

namespace {

FlowLedger::Limits ledger_limits(const GovernorConfig& config) {
    FlowLedger::Limits limits;
    limits.max_gap_ticks = config.max_telemetry_gap_ticks;
    limits.max_samples = config.samples_per_flow;
    limits.max_bytes_per_window = config.max_bytes_per_window;
    limits.max_cumulative_bytes = config.max_cumulative_bytes;
    return limits;
}

struct BindingResolution {
    bool resolved{false};
    ResourceId resource{};
    const ResourceCapacity* capacity{nullptr};
};

/// The binding resource of a path is the resource with the least unreserved
/// capacity. Ties break on ascending resource identifier, so the choice is
/// deterministic and explainable.
BindingResolution resolve_binding(const PathDescriptor* path, const CapacityTable& capacities,
                                  bool require_fresh, Tick now) {
    BindingResolution out;
    if (path == nullptr) {
        return out;
    }
    if (require_fresh && !is_fresh(path->validity, now)) {
        return out;
    }
    for (const ResourceId resource : path->resources) {
        const ResourceCapacity* candidate = capacities.find(resource);
        if (candidate == nullptr) {
            continue;
        }
        if (require_fresh && !is_fresh(candidate->validity, now)) {
            continue;
        }
        const StatusOr<KiloTickRate> available = candidate->available();
        if (!available.ok()) {
            continue;
        }
        if (!out.resolved) {
            out.resolved = true;
            out.resource = resource;
            out.capacity = candidate;
            continue;
        }
        const StatusOr<KiloTickRate> best = out.capacity->available();
        const u64 best_available = best.ok() ? best.value() : 0;
        if (available.value() < best_available ||
            (available.value() == best_available && resource < out.resource)) {
            out.resource = resource;
            out.capacity = candidate;
        }
    }
    return out;
}

}  // namespace

StatusOr<FlowMeasurements> compose_measurements(const FlowLedger& ledger,
                                                const PathDescriptor* path,
                                                const CapacityTable& capacities,
                                                u32 competing_flows,
                                                u32 competing_elephants,
                                                bool require_capacity) {
    FlowMeasurements out = ledger.measurements();
    out.path_known = path != nullptr;
    out.competing_flows = competing_flows;
    out.competing_elephants = competing_elephants;

    if (path == nullptr) {
        if (require_capacity) {
            return make_status(StatusCode::NotFound, "flow path is not present in the path table");
        }
        return out;
    }

    const BindingResolution binding = resolve_binding(path, capacities, false, 0);
    if (!binding.resolved) {
        if (require_capacity) {
            return make_status(StatusCode::NotFound,
                               "no capacity snapshot covers the flow's binding resource");
        }
        return out;
    }

    out.capacity_known = true;
    out.binding_resource = binding.resource;
    out.resource_capacity = binding.capacity->capacity;
    out.resource_reserved = binding.capacity->reserved;
    const StatusOr<KiloTickRate> available = binding.capacity->available();
    out.resource_available = available.ok() ? available.value() : 0;

    out.share_of_capacity = to_basis_points(out.sustained_rate, out.resource_capacity);
    if (out.resource_available > 0) {
        out.share_of_available =
            to_basis_points(min_of(out.sustained_rate, out.resource_available), out.resource_available);
    } else {
        out.share_of_available = out.sustained_rate > 0 ? kBasisPointsScale : 0;
    }
    return out;
}

Governor::Governor(GovernorConfig config) : config_(config) {
    if (config_.samples_per_flow < 2) {
        config_.samples_per_flow = 2;
    }
    if (config_.samples_per_flow > limits::kMaxSamplesPerFlow) {
        config_.samples_per_flow = limits::kMaxSamplesPerFlow;
    }
    if (config_.max_flows == 0) {
        config_.max_flows = 1;
    }
    if (config_.max_flows > limits::kMaxFlows) {
        config_.max_flows = limits::kMaxFlows;
    }
    if (config_.max_history_per_flow == 0) {
        config_.max_history_per_flow = 1;
    }
    if (config_.max_history_per_flow > limits::kMaxHistoryPerFlow) {
        config_.max_history_per_flow = limits::kMaxHistoryPerFlow;
    }
    if (config_.max_trace_entries == 0 || config_.max_trace_entries > limits::kMaxTraceEntries) {
        config_.max_trace_entries = limits::kMaxTraceEntries;
    }
    if (config_.classification_validity_ticks == 0) {
        config_.classification_validity_ticks = 1;
    }
    if (config_.intent_duration_ticks == 0) {
        config_.intent_duration_ticks = 1;
    }
    if (config_.max_intents == 0 || config_.max_intents > limits::kMaxIntents) {
        config_.max_intents = limits::kMaxIntents;
    }
    intents_ = IntentLedger(IntentLedger::Limits{config_.max_intents, limits::kMaxAttempts});
}

Governor::~Governor() = default;

// --- Authoritative inputs ---------------------------------------------------

Status Governor::submit_policy(PolicyDocument policy) {
    EFG_TRY(policy.validate());
    if (policy_.has_value()) {
        const PolicyDocument& existing = policy_.value();
        if (policy.generation < existing.generation) {
            ++counters_.policy_rejected;
            return make_status(StatusCode::Stale, "policy generation is older than the retained one");
        }
        if (policy.generation == existing.generation) {
            if (policy.digest != existing.digest) {
                ++counters_.policy_rejected;
                return make_status(StatusCode::Conflict,
                                   "same policy generation with different content");
            }
            if (policy.validity.issued_at < existing.validity.issued_at) {
                ++counters_.policy_rejected;
                return make_status(StatusCode::Stale,
                                   "policy re-issue is older than the retained one");
            }
            policy_ = policy;
            ++counters_.policy_accepted;
            return {};
        }
    }

    StatusOr<RuleSet> exit_rules = effective_exit_rule(policy);
    if (!exit_rules.ok()) {
        ++counters_.policy_rejected;
        return exit_rules.status();
    }

    const bool changed = policy_.has_value();
    // History is monotone in the decision tick, so the invalidation is stamped at
    // the later of the policy issue tick and the last decision this governor made.
    const Tick issued = max_of(policy.validity.issued_at, last_tick_);
    policy_ = policy;
    exit_rules_ = std::move(exit_rules.value());
    ++counters_.policy_accepted;

    if (changed) {
        // A new policy generation invalidates every classification that the
        // previous one justified. Authority is revoked now rather than being
        // carried forward under a policy that never authorized it.
        EFG_TRY(invalidate_all_authority(issued, ReasonCode::PolicyChanged));
    }
    return {};
}

Status Governor::submit_capacity(ResourceCapacity snapshot) {
    const Status status = capacities_.submit(snapshot);
    if (!status.ok()) {
        ++counters_.capacity_rejected;
        return status;
    }
    ++counters_.capacity_accepted;
    return {};
}

Status Governor::submit_path(PathDescriptor descriptor) {
    const Status status = paths_.submit(descriptor);
    if (!status.ok()) {
        ++counters_.path_rejected;
        return status;
    }
    ++counters_.path_accepted;
    return {};
}

bool Governor::evict_one_completed_flow() {
    auto victim = flows_.end();
    for (auto it = flows_.begin(); it != flows_.end(); ++it) {
        if (!it->second.ledger.completed()) {
            continue;
        }
        if (victim == flows_.end()) {
            victim = it;
            continue;
        }
        const FlowRecord& candidate = it->second;
        const FlowRecord& current = victim->second;
        if (candidate.completed_tick < current.completed_tick ||
            (candidate.completed_tick == current.completed_tick && it->first < victim->first)) {
            victim = it;
        }
    }
    if (victim == flows_.end()) {
        return false;
    }
    const FlowKey key = victim->first;
    update_index(key, victim->second);
    flows_.erase(victim);
    ++counters_.flow_evictions;
    return true;
}

Status Governor::ensure_flow_capacity(FlowId flow) {
    (void)flow;
    if (flows_.size() < config_.max_flows) {
        return {};
    }
    if (evict_one_completed_flow()) {
        return {};
    }
    ++counters_.capacity_refusals;
    return make_status(StatusCode::CapacityExceeded,
                       "flow population ceiling reached and no completed flow can be retired");
}

Governor::FlowRecord& Governor::create_flow(FlowId flow, Generation generation, bool& created) {
    FlowKey key{flow, generation};
    FlowRecord record;
    record.ledger = FlowLedger(flow, generation, ledger_limits(config_));
    record.history = ClassificationHistory(config_.max_history_per_flow);
    auto inserted = flows_.emplace(key, std::move(record));
    highest_generation_[flow] = generation;
    created = true;
    return inserted.first->second;
}

StatusOr<FlowDecision> Governor::submit_evidence(const FlowSample& sample) {
    if (!policy_.has_value()) {
        ++counters_.evidence_rejected;
        return make_status(StatusCode::NotFound, "no policy has been submitted");
    }
    // Structure is checked before any state changes: a malformed or tampered
    // sample must not create a flow record, consume population budget or move a
    // counter that a later decision depends on.
    const Status structural = sample.validate();
    if (!structural.ok()) {
        ++counters_.evidence_rejected;
        if (structural.code() == StatusCode::ContradictoryEvidence) {
            ++counters_.evidence_contradictory;
        }
        return structural;
    }

    const FlowKey key{sample.flow, sample.flow_generation};
    auto existing = flows_.find(key);
    if (existing == flows_.end()) {
        const auto highest = highest_generation_.find(sample.flow);
        if (highest != highest_generation_.end()) {
            if (sample.flow_generation < highest->second) {
                ++counters_.evidence_rejected;
                ++counters_.stale_refusals;
                return make_status(StatusCode::Stale, "evidence arrives for a retired flow generation");
            }
            if (sample.flow_generation == highest->second) {
                ++counters_.evidence_rejected;
                ++counters_.stale_refusals;
                return make_status(StatusCode::Stale,
                                   "evidence arrives for a flow generation that was retired");
            }
            EFG_TRY(ensure_flow_capacity(sample.flow));
            bool created = false;
            (void)create_flow(sample.flow, sample.flow_generation, created);
            existing = flows_.find(key);
            ++counters_.flow_generation_rollovers;
            u64 fenced = 0;
            EFG_TRY(intents_.fence_other_generations(sample.flow, sample.flow_generation, 0, fenced));
            counters_.intents_fenced += fenced;
        } else {
            EFG_TRY(ensure_flow_capacity(sample.flow));
            bool created = false;
            (void)create_flow(sample.flow, sample.flow_generation, created);
            existing = flows_.find(key);
        }
    }

    if (existing == flows_.end()) {
        return make_status(StatusCode::Internal, "flow record could not be established");
    }

    FlowRecord& record = existing->second;
    StatusOr<AppendOutcome> appended = record.ledger.append(sample);
    if (!appended.ok()) {
        if (appended.code() == StatusCode::DuplicateEvidence) {
            ++counters_.evidence_duplicates;
        } else if (appended.code() == StatusCode::ContradictoryEvidence) {
            ++counters_.evidence_contradictory;
            ++counters_.evidence_rejected;
        } else {
            ++counters_.evidence_rejected;
        }
        return appended.status();
    }

    const AppendOutcome outcome = appended.value();
    if (outcome == AppendOutcome::DuplicateIgnored) {
        ++counters_.evidence_duplicates;
    } else {
        ++counters_.evidence_accepted;
        if (record.ledger.gap_detected()) {
            ++counters_.evidence_gaps;
        }
    }
    if (record.ledger.completed() && sample.span.end > record.ledger.completed_at()) {
        ++counters_.evidence_after_completion;
    }

    Tick decision_tick = sample.span.end;
    if (sample.validity.issued_at > decision_tick) {
        decision_tick = sample.validity.issued_at;
    }
    StatusOr<FlowDecision> decision = evaluate(record, decision_tick);
    if (decision.ok()) {
        decision.value().evidence_duplicate = outcome == AppendOutcome::DuplicateIgnored;
    }
    return decision;
}

Status Governor::complete_flow(FlowId flow, Generation generation, Tick at) {
    const auto it = flows_.find(FlowKey{flow, generation});
    if (it == flows_.end()) {
        return make_status(StatusCode::NotFound, "flow generation is not present");
    }
    FlowRecord& record = it->second;
    EFG_TRY(record.ledger.mark_completed(at));
    record.completed_tick = at;
    ++counters_.flow_completions;

    u64 changed = 0;
    EFG_TRY(intents_.transition_flow_terminal(flow, generation, IntentState::Completed, at, changed));
    counters_.intents_revoked += changed;

    StatusOr<FlowDecision> decision = evaluate(record, at);
    if (!decision.ok()) {
        return decision.status();
    }
    return {};
}

// --- Indices ----------------------------------------------------------------

void Governor::update_index(const FlowKey& key, FlowRecord& record) {
    if (record.bound) {
        const auto outer = binding_index_.find(record.bound_resource);
        if (outer != binding_index_.end()) {
            const auto entry = outer->second.find(key);
            if (entry != outer->second.end()) {
                const auto aggregate = resource_aggregate_.find(record.bound_resource);
                if (aggregate != resource_aggregate_.end()) {
                    ResourceAggregate& agg = aggregate->second;
                    if (agg.flows > 0) {
                        --agg.flows;
                    }
                    if (entry->second.elephant) {
                        if (agg.elephants > 0) {
                            --agg.elephants;
                        }
                        agg.elephant_share_sum =
                            agg.elephant_share_sum >= entry->second.share
                                ? agg.elephant_share_sum - entry->second.share
                                : 0;
                    }
                    if (agg.flows == 0) {
                        resource_aggregate_.erase(aggregate);
                    }
                }
                outer->second.erase(entry);
                if (outer->second.empty()) {
                    binding_index_.erase(outer);
                }
            }
        }
        record.bound = false;
    }

    if (!record.has_classification) {
        return;
    }
    const FlowMeasurements& m = record.classification.measurements;
    if (!m.capacity_known) {
        return;
    }
    const bool elephant = record.classification.state == ElephantState::Elephant;
    record.bound_resource = m.binding_resource;
    record.bound_share = m.share_of_capacity;
    record.bound_elephant = elephant;
    record.bound = true;

    binding_index_[record.bound_resource][key] = BindingEntry{elephant, record.bound_share};
    ResourceAggregate& agg = resource_aggregate_[record.bound_resource];
    ++agg.flows;
    if (elephant) {
        ++agg.elephants;
        agg.elephant_share_sum += record.bound_share;
    }
}

Status Governor::invalidate_all_authority(Tick now, ReasonCode reason) {
    for (auto& entry : flows_) {
        FlowRecord& record = entry.second;
        const bool was_elephant = record.has_classification &&
                                  record.classification.state == ElephantState::Elephant;
        if (record.has_classification) {
            const ElephantState from = record.classification.state;
            ReasonCode new_reasons = record.classification.reasons | reason |
                                     ReasonCode::RevalidationRequired;
            if (from == ElephantState::Elephant) {
                ClassificationTransition transition;
                transition.at = now;
                transition.from = from;
                transition.to = ElephantState::Suspended;
                transition.authority = AuthorityLevel::None;
                transition.reasons = new_reasons;
                transition.classification = record.classification.id;
                transition.classification_generation = record.classification.generation;
                EFG_TRY(record.history.record(transition));
                record.classification.state = ElephantState::Suspended;
                record.classification.authority = AuthorityLevel::None;
                record.classification.reasons = new_reasons;
                ++counters_.authority_revocations;
            } else {
                record.classification.reasons = new_reasons;
            }
            record.enter_streak = 0;
            record.exit_streak = 0;
        }
        if (was_elephant) {
            update_index(entry.first, record);
        }
    }
    u64 changed = 0;
    EFG_TRY(intents_.revoke_all(now, IntentState::Revoked, SuppressReason::AuthorityStale, changed));
    counters_.intents_revoked += changed;
    return {};
}

// --- Evaluation -------------------------------------------------------------

ReasonCode Governor::suspension_reasons(const FlowRecord& record, Tick now, bool& suspend) const {
    suspend = false;
    ReasonCode reasons = ReasonCode::None;
    const FlowSample* latest = record.ledger.latest();

    if (latest == nullptr) {
        reasons |= ReasonCode::EvidenceMissing;
        suspend = true;
    } else {
        if (evaluate_freshness(record.ledger.latest_validity(), now) != FreshnessVerdict::Fresh) {
            reasons |= ReasonCode::EvidenceStale;
            suspend = true;
        }
        if (record.ledger.gap_detected()) {
            reasons |= ReasonCode::TelemetryGap;
            suspend = true;
        }
        const PathDescriptor* path = paths_.find(latest->path);
        if (path == nullptr) {
            reasons |= ReasonCode::PathUnknown;
            suspend = true;
        } else if (!is_fresh(path->validity, now)) {
            reasons |= ReasonCode::PathUnknown;
            reasons |= ReasonCode::EvidenceStale;
            suspend = true;
        } else {
            // Capacity evidence is only authoritative inside its validity window.
            // An expired snapshot cannot justify a share of resource.
            const BindingResolution binding = resolve_binding(path, capacities_, true, now);
            if (!binding.resolved) {
                reasons |= ReasonCode::CapacityStale;
                reasons |= ReasonCode::CapacityMissing;
                suspend = true;
            }
        }
    }

    if (!policy_.has_value()) {
        reasons |= ReasonCode::PolicyChanged;
        suspend = true;
    } else if (evaluate_freshness(policy_->validity, now) != FreshnessVerdict::Fresh) {
        reasons |= ReasonCode::PolicyChanged;
        suspend = true;
    }

    if (record.has_classification && record.classification.state == ElephantState::Elephant) {
        const StatusOr<AuthorityVector> live = build_authority(record, now);
        if (!live.ok()) {
            suspend = true;
            reasons |= ReasonCode::RevalidationRequired;
        } else {
            const BindingVerdict verdict =
                record.classification.authority_vector.compare(live.value());
            if (verdict != BindingVerdict::Match) {
                suspend = true;
                reasons |= ReasonCode::RevalidationRequired;
                switch (verdict) {
                    case BindingVerdict::FlowGenerationChanged:
                        reasons |= ReasonCode::FlowGenerationChanged;
                        break;
                    case BindingVerdict::PathGenerationChanged:
                        reasons |= ReasonCode::PathGenerationChanged;
                        break;
                    case BindingVerdict::PolicyGenerationChanged:
                        reasons |= ReasonCode::PolicyChanged;
                        break;
                    case BindingVerdict::CapacityGenerationChanged:
                        reasons |= ReasonCode::CapacityStale;
                        break;
                    case BindingVerdict::PublisherChanged:
                    case BindingVerdict::PublisherIncarnationChanged:
                    case BindingVerdict::EpochChanged:
                    case BindingVerdict::BootChanged:
                    case BindingVerdict::Absent:
                        reasons |= ReasonCode::EvidenceStale;
                        break;
                    case BindingVerdict::EvidenceWindowSuperseded:
                    case BindingVerdict::Match:
                    default:
                        break;
                }
            }
        }
    }
    // Withdrawing a qualified classification always requires revalidation before
    // authority can be re-earned, whatever the trigger was.
    if (suspend && record.has_classification &&
        record.classification.state == ElephantState::Elephant) {
        reasons |= ReasonCode::RevalidationRequired;
    }
    return reasons;
}

StatusOr<AuthorityVector> Governor::build_authority(const FlowRecord& record, Tick now) const {
    AuthorityVector authority;
    authority.flow = record.ledger.flow();
    authority.flow_generation = record.ledger.generation();
    authority.epoch = incarnation_.boot.epoch;
    authority.boot = incarnation_.boot.boot;
    authority.bound_flags = kAuthorityFlowBound | kAuthorityPolicyBound | kAuthorityEpochBound |
                            kAuthorityBootBound;

    const FlowSample* latest = record.ledger.latest();
    if (latest != nullptr) {
        authority.path = latest->path;
        authority.path_generation = latest->path_generation;
        authority.window = latest->window;
        authority.window_sequence = latest->window_sequence;
        authority.publisher = latest->provenance.publisher.publisher;
        authority.publisher_boot = latest->provenance.publisher.boot;
        authority.bound_flags |=
            kAuthorityPathBound | kAuthorityWindowBound | kAuthorityPublisherBound;
    }
    if (policy_.has_value()) {
        authority.policy = policy_->id;
        authority.policy_generation = policy_->generation;
    }

    const PathDescriptor* path = latest != nullptr ? paths_.find(latest->path) : nullptr;
    const BindingResolution binding = resolve_binding(path, capacities_, false, now);
    if (binding.resolved) {
        authority.capacity = binding.capacity->snapshot;
        authority.capacity_generation = binding.capacity->generation;
        authority.bound_flags |= kAuthorityCapacityBound;
    }
    StatusOr<ValidityWindow> validity = make_validity(now, config_.classification_validity_ticks);
    if (!validity.ok()) {
        return validity.status();
    }
    authority.validity = validity.value();
    return authority;
}

Status Governor::enforce_protection(FlowClassification& cls) const {
    if (!policy_.has_value() || !policy_->authorization.protect_obligations) {
        return {};
    }
    const FlowMeasurements& m = cls.measurements;
    if (m.protection == ProtectionState::Unknown) {
        // An unknown protection state cannot rule out a protected obligation, so
        // the classification observes but never governs. UNKNOWN stays UNKNOWN.
        cls.reasons |= ReasonCode::EvidenceUnknownField;
        cls.reasons |= ReasonCode::ProtectedObligation;
        if (cls.authority == AuthorityLevel::Govern) {
            cls.authority = AuthorityLevel::Observe;
        }
        return {};
    }
    bool obligation = false;
    if (m.protection == ProtectionState::Protected) {
        cls.reasons |= ReasonCode::ProtectedObligation;
        obligation = true;
    }
    if (m.protection == ProtectionState::NonPreemptible) {
        cls.reasons |= ReasonCode::ProtectedObligation;
        cls.reasons |= ReasonCode::NonPreemptibleObligation;
        obligation = true;
    }
    if (m.has_reservation) {
        cls.reasons |= ReasonCode::ProtectedObligation;
        cls.reasons |= ReasonCode::ReservedCapacityBounded;
        obligation = true;
    }
    if (m.service_class == ServiceClass::Reserved || m.service_class == ServiceClass::Control) {
        cls.reasons |= ReasonCode::ProtectedObligation;
        cls.reasons |= ReasonCode::ControlClassBounded;
        obligation = true;
    }
    if (obligation && cls.authority == AuthorityLevel::Govern) {
        cls.authority = AuthorityLevel::Observe;
    }
    return {};
}

StatusOr<FlowDecision> Governor::evaluate(FlowRecord& record, Tick now) {
    if (record.has_classification && now < record.evaluated_tick) {
        ++counters_.stale_refusals;
        return make_status(StatusCode::Stale,
                           "decision tick regressed below the flow's last decision");
    }

    FlowDecision decision;
    const FlowId flow = record.ledger.flow();
    const Generation generation = record.ledger.generation();
    const FlowSample* latest = record.ledger.latest();
    const PathDescriptor* path = latest != nullptr ? paths_.find(latest->path) : nullptr;

    bool suspend = false;
    ReasonCode reasons = suspension_reasons(record, now, suspend);

    FlowClassification candidate;
    candidate.flow = flow;
    candidate.flow_generation = generation;
    candidate.decided_at = now;

    // --- Measurements -------------------------------------------------------
    FlowMeasurements measurements;
    u32 competing_elephants = 0;
    BasisPoints contention = 0;
    {
        const auto aggregate = resource_aggregate_.find(record.bound_resource);
        u32 competing_flows = 0;
        if (record.bound && aggregate != resource_aggregate_.end()) {
            competing_flows = aggregate->second.flows;
            competing_elephants = aggregate->second.elephants;
            if (competing_flows > 0) {
                --competing_flows;
            }
            if (record.bound_elephant && competing_elephants > 0) {
                --competing_elephants;
            }
        }
        StatusOr<FlowMeasurements> composed = compose_measurements(
            record.ledger, path, capacities_, competing_flows, competing_elephants, true);
        if (composed.ok()) {
            measurements = composed.value();
        } else {
            measurements = record.ledger.measurements();
            measurements.path_known = path != nullptr;
            measurements.competing_flows = competing_flows;
            measurements.competing_elephants = competing_elephants;
            reasons |= ReasonCode::CapacityMissing;
            suspend = true;
        }
        if (measurements.evidence_present && !measurements.evidence_contiguous) {
            reasons |= ReasonCode::EvidenceUnknownField;
        }
        if (measurements.capacity_known) {
            const auto cap_aggregate = resource_aggregate_.find(measurements.binding_resource);
            if (cap_aggregate != resource_aggregate_.end()) {
                u64 sum = cap_aggregate->second.elephant_share_sum;
                if (record.bound && record.bound_elephant &&
                    measurements.binding_resource == record.bound_resource) {
                    sum = sum >= record.bound_share ? sum - record.bound_share : 0;
                }
                contention = static_cast<BasisPoints>(sum > kBasisPointsScale ? kBasisPointsScale : sum);
            }
        }
    }

    const bool new_evidence = latest != nullptr && latest->window != record.evaluated_window;
    const ElephantState previous_state =
        record.has_classification ? record.classification.state : ElephantState::Unknown;

    if (record.ledger.completed()) {
        // A completed flow is definitively not an elephant any more. This is a
        // negative classification rather than an unknown one: the flow is gone.
        candidate.state = ElephantState::NotElephant;
        candidate.authority = AuthorityLevel::None;
        reasons |= ReasonCode::FlowCompleted;
        candidate.enter_streak = 0;
        candidate.exit_streak = 0;
    } else if (suspend) {
        // Suspended is a pending revalidation state, not a one shot: once
        // authority has been withdrawn the flow stays suspended until fresh
        // evidence re-qualifies it.
        const bool pending = record.has_classification &&
                             (previous_state == ElephantState::Elephant ||
                              previous_state == ElephantState::Suspended);
        candidate.state = pending ? ElephantState::Suspended : ElephantState::Unknown;
        candidate.authority = AuthorityLevel::None;
        candidate.enter_streak = 0;
        candidate.exit_streak = 0;
    } else {
        ThresholdTrace trace;
        const Tri enter = policy_->enter_rule.evaluate(measurements, &trace);
        const Tri exit = exit_rules_.evaluate(measurements, nullptr);

        // The exit rule states that the flow *still* satisfies the weakened
        // thresholds. Leaving the elephant state therefore requires the exit rule
        // to be definitively false for exit_confirm_windows consecutive windows.
        // Unknown keeps the classification, which is the fail-safe direction.
        const bool below_exit = exit == Tri::False;

        candidate.enter_streak = record.enter_streak;
        candidate.exit_streak = record.exit_streak;
        if (new_evidence) {
            if (previous_state == ElephantState::Elephant) {
                candidate.exit_streak = below_exit ? record.exit_streak + 1 : 0;
                candidate.enter_streak = record.enter_streak;
            } else {
                candidate.enter_streak = tri_is_true(enter) ? record.enter_streak + 1 : 0;
                candidate.exit_streak = 0;
            }
        }

        if (previous_state == ElephantState::Elephant) {
            if (below_exit &&
                candidate.exit_streak >= policy_->hysteresis.exit_confirm_windows) {
                candidate.state = ElephantState::NotElephant;
                candidate.authority = AuthorityLevel::None;
                reasons |= ReasonCode::PolicyRuleNotSatisfied;
                reasons |= ReasonCode::BelowRateThreshold;
            } else {
                candidate.state = ElephantState::Elephant;
                reasons |= ReasonCode::PolicyRuleSatisfied;
                reasons |= below_exit ? ReasonCode::HysteresisHeld
                                      : ReasonCode::ExitThresholdNotReached;
            }
        } else if (!new_evidence && previous_state == ElephantState::Suspended) {
            // Revalidation requires fresh evidence. A re-evaluation that observes
            // nothing new neither restores nor discards the suspension.
            candidate.state = ElephantState::Suspended;
            candidate.authority = AuthorityLevel::None;
            // The reasons that caused the suspension still describe it, and
            // nothing observed since has changed them.
            reasons |= record.classification.reasons;
            reasons |= ReasonCode::RevalidationRequired;
        } else if (tri_is_true(enter)) {
            reasons |= ReasonCode::PolicyRuleSatisfied;
            if (candidate.enter_streak >= policy_->hysteresis.enter_confirm_windows) {
                candidate.state = ElephantState::Elephant;
            } else {
                candidate.state = ElephantState::NotElephant;
                reasons |= ReasonCode::HysteresisHeld;
            }
        } else if (tri_is_unknown(enter)) {
            reasons |= ReasonCode::PolicyRuleIndeterminate;
            if (!measurements.evidence_present) {
                reasons |= ReasonCode::EvidenceMissing;
            }
            if (!measurements.capacity_known) {
                reasons |= ReasonCode::CapacityMissing;
            }
            if (previous_state == ElephantState::Suspended) {
                // A flow awaiting revalidation is not reclassified on evidence
                // that cannot decide the question. It stays suspended.
                candidate.state = ElephantState::Suspended;
                candidate.authority = AuthorityLevel::None;
                reasons |= record.classification.reasons;
                reasons |= ReasonCode::RevalidationRequired;
            } else {
                candidate.state = ElephantState::Unknown;
            }
        } else {
            candidate.state = ElephantState::NotElephant;
            reasons |= ReasonCode::PolicyRuleNotSatisfied;
            candidate.enter_streak = 0;
        }

        for (const ThresholdTraceEntry& entry : trace.entries) {
            if (entry.result == Tri::True) {
                switch (entry.kind) {
                    case ThresholdKind::CumulativeBytes:
                        reasons |= ReasonCode::VolumeThresholdMet;
                        break;
                    case ThresholdKind::DurationTicks:
                        reasons |= ReasonCode::DurationThresholdMet;
                        break;
                    case ThresholdKind::SustainedRate:
                    case ThresholdKind::PeakRate:
                        reasons |= ReasonCode::SustainedRateThresholdMet;
                        break;
                    case ThresholdKind::ShareOfCapacity:
                    case ThresholdKind::ShareOfAvailable:
                    case ThresholdKind::ExcessOverGuarantee:
                        reasons |= ReasonCode::ResourceShareThresholdMet;
                        break;
                    default:
                        break;
                }
            } else if (entry.result == Tri::False) {
                switch (entry.kind) {
                    case ThresholdKind::CumulativeBytes:
                        reasons |= ReasonCode::BelowVolumeThreshold;
                        break;
                    case ThresholdKind::DurationTicks:
                        reasons |= ReasonCode::BelowDurationThreshold;
                        break;
                    case ThresholdKind::SustainedRate:
                    case ThresholdKind::PeakRate:
                        reasons |= ReasonCode::BelowRateThreshold;
                        break;
                    case ThresholdKind::ShareOfCapacity:
                    case ThresholdKind::ShareOfAvailable:
                    case ThresholdKind::ExcessOverGuarantee:
                        reasons |= ReasonCode::BelowShareThreshold;
                        break;
                    default:
                        break;
                }
            }
        }
        candidate.trace = trace.entries;
        candidate.trace_truncated = trace.truncated;
    }

    if (candidate.state == ElephantState::Elephant) {
        candidate.authority = AuthorityLevel::Govern;
    }
    candidate.measurements = measurements;
    if (policy_.has_value()) {
        candidate.impact =
            compute_impact(measurements, policy_.value(), contention, competing_elephants);
    }
    candidate.reasons = reasons;
    EFG_TRY(enforce_protection(candidate));

    // --- Idempotency. An unchanged decision for unchanged evidence is returned
    // as it stands, so replaying a window cannot mint fresh authority.
    if (record.has_classification && !new_evidence &&
        record.classification.state == candidate.state &&
        record.classification.authority == candidate.authority &&
        record.classification.reasons == candidate.reasons &&
        record.classification.measurements.digest() == candidate.measurements.digest() &&
        record.classification.impact.digest() == candidate.impact.digest()) {
        record.evaluated_tick = now;
        if (now > last_tick_) {
            last_tick_ = now;
        }
        decision.classification = record.classification;
        return decision;
    }

    StatusOr<AuthorityVector> authority = build_authority(record, now);
    if (!authority.ok()) {
        return authority.status();
    }
    candidate.authority_vector = authority.value();
    if (record.has_classification && record.classification.generation.valid()) {
        if (record.classification.state == candidate.state &&
            record.classification.authority == candidate.authority) {
            candidate.generation = record.classification.generation;
        } else {
            StatusOr<Generation> next = record.classification.generation.next();
            if (!next.ok()) {
                return next.status();
            }
            candidate.generation = next.value();
        }
    } else {
        candidate.generation = Generation::initial();
    }
    // The authority vector names the classification generation it belongs to, so
    // an intent can be tied to the exact decision that authorized it.
    candidate.authority_vector.classification_generation = candidate.generation;
    if (classification_sequence_ == UINT64_MAX) {
        return make_status(StatusCode::Overflow, "classification identifier space is exhausted");
    }
    ++classification_sequence_;
    candidate.id = ClassificationId{classification_sequence_};

    ClassificationTransition transition;
    transition.at = now;
    transition.from = previous_state;
    transition.to = candidate.state;
    transition.authority = candidate.authority;
    transition.reasons = candidate.reasons;
    transition.classification = candidate.id;
    transition.classification_generation = candidate.generation;
    EFG_TRY(record.history.record(transition));
    candidate.history_digest = record.history.digest();
    candidate.digest = candidate.compute_digest();

    const bool state_changed = !record.has_classification || previous_state != candidate.state;
    const bool authority_changed =
        !record.has_classification || record.classification.authority != candidate.authority;
    if (state_changed) {
        ++counters_.transitions;
    }

    record.classification = candidate;
    record.has_classification = true;
    record.evaluated_tick = now;
    record.evaluated_window = latest != nullptr ? latest->window : EvidenceWindowId{};
    record.enter_streak = candidate.enter_streak;
    record.exit_streak = candidate.exit_streak;

    ++counters_.decisions;
    switch (candidate.state) {
        case ElephantState::Elephant:
            ++counters_.elephant_decisions;
            break;
        case ElephantState::NotElephant:
            ++counters_.not_elephant_decisions;
            break;
        case ElephantState::Unknown:
            ++counters_.unknown_decisions;
            break;
        case ElephantState::Suspended:
            ++counters_.suspended_decisions;
            break;
    }
    if (candidate.state == ElephantState::Suspended) {
        ++counters_.authority_revocations;
        ++counters_.revalidations;
        u64 revoked = 0;
        EFG_TRY(intents_.revoke_flow(flow, generation, now, revoked));
        counters_.intents_revoked += revoked;
    }

    update_index(FlowKey{flow, generation}, record);

    if (now > last_tick_) {
        last_tick_ = now;
    }
    decision.classification = candidate;
    decision.state_changed = state_changed;
    decision.authority_changed = authority_changed;
    EFG_TRY(produce_intents(record, decision.intents, now));
    return decision;
}

// --- Governance intent ------------------------------------------------------

StatusOr<IntentBounds> Governor::ceiling_bounds(Tick now) const {
    IntentBounds ceiling;
    if (!policy_.has_value()) {
        return ceiling;
    }
    const IntentAuthorization& auth = policy_->authorization;
    ceiling.rate_reduction_bp = auth.max_shaping_reduction_bp;
    ceiling.duration_ticks = min_of(config_.intent_duration_ticks, auth.max_intent_duration_ticks);
    ceiling.targets = auth.max_targets;
    ceiling.placement = auth.allow_alternate_placement;
    ceiling.isolation = auth.allow_scheduling_isolation;
    ceiling.admission_reduction = auth.allow_competing_admission_reduction;
    ceiling.congestion_escalation = auth.allow_congestion_escalation;
    (void)now;
    return ceiling;
}

Status Governor::produce_intents(FlowRecord& record, std::vector<GovernanceIntent>& out,
                                 Tick now) {
    out.clear();
    if (!policy_.has_value() || !record.has_classification) {
        return {};
    }
    const FlowClassification& cls = record.classification;
    const IntentAuthorization& auth = policy_->authorization;
    const FlowId flow = cls.flow;
    const Generation generation = cls.flow_generation;

    // Retire every live intent whose authorizing classification generation moved.
    {
        u64 revoked = 0;
        EFG_TRY(intents_.revoke_stale_generation(flow, generation, cls.generation, now, revoked));
        counters_.intents_revoked += revoked;
    }

    if (cls.state != ElephantState::Elephant || record.ledger.completed()) {
        return {};
    }

    const IntentBounds ceiling = ceiling_bounds(now).value_or(IntentBounds{});
    const FlowMeasurements& m = cls.measurements;
    const Tick window_end =
        saturating_add(now, min_of(config_.intent_duration_ticks, ceiling.duration_ticks));
    const std::vector<GovernanceIntent> live = intents_.for_flow(flow, generation);

    std::size_t live_count = 0;
    for (const GovernanceIntent& existing : live) {
        if (is_live(existing.state)) {
            ++live_count;
        }
    }

    const auto already_live = [&live, &cls, now](IntentKind kind) {
        for (const GovernanceIntent& existing : live) {
            if (existing.kind == kind && is_live(existing.state) &&
                existing.classification_generation == cls.generation &&
                now >= existing.window.begin && now < existing.window.end) {
                return true;
            }
        }
        return false;
    };

    const auto emit = [&](IntentKind kind, const IntentBounds& requested, IntentState state,
                          SuppressReason suppressed) -> Status {
        GovernanceIntent intent;
        intent.kind = kind;
        intent.flow = flow;
        intent.flow_generation = generation;
        intent.classification = cls.id;
        intent.classification_generation = cls.generation;
        intent.state = state;
        intent.suppressed = suppressed;
        intent.requested = requested;
        intent.granted = state == IntentState::Suppressed ? IntentBounds{}
                                                         : narrow_bounds(requested, ceiling);
        intent.severity = cls.impact.severity;
        intent.contention_pressure = cls.impact.contention_pressure;
        intent.competing_elephants = cls.impact.competing_elephants;
        intent.window = TickSpan{now, window_end};
        intent.authority = cls.authority_vector;
        intent.provenance = Provenance{};
        if (policy_.has_value()) {
            intent.provenance = policy_->provenance;
        }
        Hasher hasher;
        hasher.write_u64(flow.value());
        hasher.write_u64(generation.value());
        hasher.write_u8(static_cast<u8>(kind));
        hasher.write_u64(cls.generation.value());
        hasher.write_u64(cls.id.value());
        hasher.write_u64(now);
        u64 attempt = hasher.finish();
        if (attempt == 0) {
            attempt = 1;
        }
        intent.attempt = AttemptId{attempt};

        StatusOr<GovernanceIntent> stored = intents_.record(intent);
        if (!stored.ok()) {
            if (stored.code() == StatusCode::CapacityExceeded) {
                GovernanceIntent rejected = intent;
                rejected.state = IntentState::Suppressed;
                rejected.suppressed = SuppressReason::LedgerBudgetExhausted;
                rejected.granted = IntentBounds{};
                rejected.digest = rejected.compute_digest();
                out.push_back(rejected);
                ++counters_.intents_suppressed;
                return {};
            }
            return stored.status();
        }
        ++counters_.intents_proposed;
        switch (stored.value().state) {
            case IntentState::Clamped:
                ++counters_.intents_clamped;
                break;
            case IntentState::Suppressed:
                ++counters_.intents_suppressed;
                break;
            case IntentState::Authorized:
                ++counters_.intents_authorized;
                break;
            default:
                break;
        }
        out.push_back(stored.value());
        return {};
    };

    // Protection is declarative: it never changes anything, but it records that
    // a protected high-volume flow was recognised and deliberately untouched.
    if (cls.is_protected() && !already_live(IntentKind::ProtectReservedFlow)) {
        IntentBounds requested;
        requested.duration_ticks = config_.intent_duration_ticks;
        EFG_TRY(emit(IntentKind::ProtectReservedFlow, requested, IntentState::Authorized,
                     SuppressReason::None));
    }

    if (cls.authority != AuthorityLevel::Govern) {
        return {};
    }
    if (!m.capacity_known) {
        if (!already_live(IntentKind::ObserveOnly)) {
            IntentBounds requested;
            requested.duration_ticks = config_.intent_duration_ticks;
            EFG_TRY(emit(IntentKind::ObserveOnly, requested, IntentState::Suppressed,
                         SuppressReason::CapacityUnknown));
        }
        return {};
    }
    if (cls.impact.severity < auth.min_impact_severity) {
        if (!already_live(IntentKind::ObserveOnly)) {
            IntentBounds requested;
            requested.duration_ticks = config_.intent_duration_ticks;
            EFG_TRY(emit(IntentKind::ObserveOnly, requested, IntentState::Suppressed,
                         SuppressReason::ImpactBelowThreshold));
        }
        return {};
    }
    if (live_count >= limits::kMaxIntentListPerFlow) {
        IntentBounds requested;
        requested.duration_ticks = config_.intent_duration_ticks;
        EFG_TRY(emit(IntentKind::ObserveOnly, requested, IntentState::Suppressed,
                     SuppressReason::LedgerBudgetExhausted));
        return {};
    }

    const u32 fair_divisor = m.competing_flows + 1u;
    const BasisPoints fair_share =
        static_cast<BasisPoints>(kBasisPointsScale / (fair_divisor == 0 ? 1u : fair_divisor));
    const BasisPoints needed_reduction =
        m.share_of_available > fair_share ? m.share_of_available - fair_share : 0;

    if (auth.allow_rate_shaping && needed_reduction > 0 &&
        !already_live(IntentKind::RequestRateShaping)) {
        IntentBounds requested;
        requested.rate_reduction_bp = needed_reduction;
        requested.duration_ticks = config_.intent_duration_ticks;
        const IntentState state = requested.rate_reduction_bp <= ceiling.rate_reduction_bp
                                      ? IntentState::Authorized
                                      : IntentState::Clamped;
        EFG_TRY(emit(IntentKind::RequestRateShaping, requested, state, SuppressReason::None));
    }

    if (auth.allow_alternate_placement && cls.impact.contention_pressure >= auth.min_contention_pressure &&
        m.competing_flows > 0 && !already_live(IntentKind::RequestAlternatePlacement)) {
        IntentBounds requested;
        requested.placement = true;
        requested.duration_ticks = config_.intent_duration_ticks;
        EFG_TRY(emit(IntentKind::RequestAlternatePlacement, requested, IntentState::Authorized,
                     SuppressReason::None));
    }

    if (auth.allow_competing_admission_reduction && cls.impact.competing_elephants > 0 &&
        cls.impact.contention_pressure >= auth.min_contention_pressure &&
        !already_live(IntentKind::ReduceCompetingAdmission)) {
        IntentBounds requested;
        requested.admission_reduction = true;
        requested.targets = cls.impact.competing_elephants;
        requested.duration_ticks = config_.intent_duration_ticks;
        const IntentState state =
            requested.targets <= ceiling.targets ? IntentState::Authorized : IntentState::Clamped;
        EFG_TRY(emit(IntentKind::ReduceCompetingAdmission, requested, state, SuppressReason::None));
    }

    if (auth.allow_scheduling_isolation && m.priority.known() && m.competing_flows > 0 &&
        cls.impact.contention_pressure >= auth.min_contention_pressure &&
        !already_live(IntentKind::RequestSchedulingIsolation)) {
        IntentBounds requested;
        requested.isolation = true;
        requested.duration_ticks = config_.intent_duration_ticks;
        EFG_TRY(emit(IntentKind::RequestSchedulingIsolation, requested, IntentState::Authorized,
                     SuppressReason::None));
    }

    if (auth.allow_congestion_escalation &&
        (cls.impact.contention_pressure >= kBasisPointsScale ||
         m.share_of_available >= kBasisPointsScale || m.resource_available == 0) &&
        !already_live(IntentKind::EscalateCongestion)) {
        IntentBounds requested;
        requested.congestion_escalation = true;
        requested.duration_ticks = config_.intent_duration_ticks;
        EFG_TRY(emit(IntentKind::EscalateCongestion, requested, IntentState::Authorized,
                     SuppressReason::None));
    }
    return {};
}

// --- Sweeps -----------------------------------------------------------------

StatusOr<DecisionBatch> Governor::tick(Tick now) {
    if (policy_.has_value() && policy_->validity.well_formed() && now < policy_->validity.issued_at) {
        ++counters_.stale_refusals;
        return make_status(StatusCode::Stale, "sweep tick precedes the active policy issue tick");
    }
    if (now < last_tick_) {
        ++counters_.stale_refusals;
        return make_status(StatusCode::Stale, "sweep tick regressed below the last committed tick");
    }
    Status observed = tick_guard_.observe(now);
    if (!observed.ok()) {
        ++counters_.stale_refusals;
        return observed;
    }

    DecisionBatch batch;
    batch.now = now;
    ++counters_.ticks;
    ++counters_.sweeps;

    EFG_TRY(capacities_.expire(now));
    EFG_TRY(paths_.expire(now));

    u64 expired = 0;
    EFG_TRY(intents_.expire(now, expired));
    counters_.intents_expired += expired;
    batch.expirations = expired;

    std::vector<FlowKey> keys;
    keys.reserve(flows_.size());
    for (const auto& entry : flows_) {
        keys.push_back(entry.first);
    }

    for (const FlowKey& key : keys) {
        const auto it = flows_.find(key);
        if (it == flows_.end()) {
            continue;
        }
        FlowRecord& record = it->second;
        if (record.has_classification && record.evaluated_tick > now) {
            continue;  // the flow is already ahead of this sweep
        }
        const bool was_elephant = record.has_classification &&
                                  record.classification.state == ElephantState::Elephant;
        bool suspend = false;
        (void)suspension_reasons(record, now, suspend);
        // A sweep only ever withdraws authority. Qualification requires fresh
        // evidence, so flows that are not currently elephants are left alone.
        if (!was_elephant && !suspend) {
            continue;
        }
        StatusOr<FlowDecision> decision = evaluate(record, now);
        if (!decision.ok()) {
            continue;
        }
        if (!decision.value().state_changed && !decision.value().authority_changed) {
            continue;
        }
        if (decision.value().classification.state == ElephantState::Suspended) {
            ++batch.suspensions;
            ++batch.revalidations;
            ++batch.revocations;
        }
        batch.decisions.push_back(std::move(decision.value()));
    }

    if (now > last_tick_) {
        last_tick_ = now;
    }
    return batch;
}

// --- Queries ----------------------------------------------------------------

StatusOr<FlowClassification> Governor::classification(FlowId flow, Generation generation, Tick now) {
    const auto it = flows_.find(FlowKey{flow, generation});
    if (it == flows_.end()) {
        return make_status(StatusCode::NotFound, "flow generation is not present");
    }
    StatusOr<FlowDecision> decision = evaluate(it->second, now);
    if (!decision.ok()) {
        return decision.status();
    }
    return decision.value().classification;
}

const FlowLedger* Governor::ledger(FlowId flow, Generation generation) const {
    const auto it = flows_.find(FlowKey{flow, generation});
    return it == flows_.end() ? nullptr : &it->second.ledger;
}

const ClassificationHistory* Governor::history(FlowId flow, Generation generation) const {
    const auto it = flows_.find(FlowKey{flow, generation});
    return it == flows_.end() ? nullptr : &it->second.history;
}

std::vector<FlowId> Governor::flow_ids() const {
    std::vector<FlowId> out;
    out.reserve(flows_.size());
    for (const auto& entry : flows_) {
        if (out.empty() || out.back() != entry.first.first) {
            out.push_back(entry.first.first);
        }
    }
    return out;
}

std::vector<std::pair<FlowId, Generation>> Governor::flow_keys() const {
    std::vector<std::pair<FlowId, Generation>> out;
    out.reserve(flows_.size());
    for (const auto& entry : flows_) {
        out.push_back(entry.first);
    }
    return out;
}

StatusOr<Explanation> Governor::explain(FlowId flow, Generation generation, Tick now) {
    const auto it = flows_.find(FlowKey{flow, generation});
    if (it == flows_.end()) {
        return make_status(StatusCode::NotFound, "flow generation is not present");
    }
    StatusOr<FlowDecision> decision = evaluate(it->second, now);
    if (!decision.ok()) {
        return decision.status();
    }
    const FlowRecord& record = it->second;
    Explanation explanation;
    explanation.found = true;
    explanation.flow = flow;
    explanation.flow_generation = generation;
    explanation.state = decision.value().classification.state;
    explanation.authority = decision.value().classification.authority;
    explanation.reasons = decision.value().classification.reasons;
    explanation.measurements = decision.value().classification.measurements;
    explanation.impact = decision.value().classification.impact;
    explanation.trace = decision.value().classification.trace;
    explanation.truncated = decision.value().classification.trace_truncated;
    explanation.authority_vector = decision.value().classification.authority_vector;
    explanation.history_digest = record.history.digest();
    explanation.history_folded = record.history.folded_count();
    explanation.intents = intents_.for_flow(flow, generation);
    return explanation;
}

void Governor::reset() noexcept {
    counters_ = GovernorCounters{};
    incarnation_ = GovernorIncarnation{};
    tick_guard_.reset();
    last_tick_ = 0;
    policy_.reset();
    exit_rules_ = RuleSet{};
    capacities_ = CapacityTable{};
    paths_ = PathTable{};
    flows_.clear();
    highest_generation_.clear();
    binding_index_.clear();
    resource_aggregate_.clear();
    intents_ = IntentLedger(IntentLedger::Limits{config_.max_intents, limits::kMaxAttempts});
    classification_sequence_ = 0;
    intent_sequence_ = 0;
}

// --- Durable state ----------------------------------------------------------

Status Governor::serialize(ByteWriter& writer) const {
    writer.write_u32(kSchemaVersion);
    writer.write_u32(1);  // serialization format revision
    writer.write_u64(last_tick_);
    writer.write_u64(classification_sequence_);
    writer.write_u64(intent_sequence_);
    writer.write_u64(incarnation_.boot.boot.value());
    writer.write_u64(incarnation_.boot.epoch.value());
    writer.write_u64(incarnation_.boot.started_at);
    writer.write_u64(incarnation_.boot.monotonic_seed);

    writer.write_bool(policy_.has_value());
    if (policy_.has_value()) {
        encode(writer, policy_.value());
    }

    writer.write_u32(static_cast<u32>(capacities_.size()));
    for (const ResourceId resource : capacities_.resources()) {
        const ResourceCapacity* capacity = capacities_.find(resource);
        if (capacity != nullptr) {
            encode(writer, *capacity);
        }
    }

    writer.write_u32(static_cast<u32>(paths_.size()));
    for (const PathId path : paths_.paths()) {
        const PathDescriptor* descriptor = paths_.find(path);
        if (descriptor != nullptr) {
            encode(writer, *descriptor);
        }
    }

    writer.write_u32(static_cast<u32>(flows_.size()));
    for (const auto& entry : flows_) {
        const FlowRecord& record = entry.second;
        writer.write_u64(entry.first.first.value());
        writer.write_u64(entry.first.second.value());
        writer.write_bool(record.has_classification);
        if (record.has_classification) {
            encode(writer, record.classification);
        }
        writer.write_u32(record.enter_streak);
        writer.write_u32(record.exit_streak);
        writer.write_u64(record.evaluated_tick);
        writer.write_u64(record.evaluated_window.value());
        writer.write_u64(record.completed_tick);
        writer.write_bool(record.ledger.completed());
        writer.write_u64(record.ledger.completed_at());
        writer.write_bool(record.ledger.gap_detected());

        const std::vector<FlowSample>& samples = record.ledger.samples();
        writer.write_u32(static_cast<u32>(samples.size()));
        for (const FlowSample& sample : samples) {
            encode(writer, sample);
        }

        const std::vector<ClassificationTransition> transitions = record.history.transitions();
        writer.write_u32(static_cast<u32>(transitions.size()));
        for (const ClassificationTransition& transition : transitions) {
            encode(writer, transition);
        }
        writer.write_u64(record.history.digest());
        writer.write_u64(record.history.folded_count());
    }

    const std::vector<GovernanceIntent> intents = intents_.all_intents();
    writer.write_u32(static_cast<u32>(intents.size()));
    for (const GovernanceIntent& intent : intents) {
        encode(writer, intent);
    }

    if (writer.overflowed()) {
        return make_status(StatusCode::Oversized, "serialized governor state exceeded the ceiling");
    }
    return {};
}

Status Governor::deserialize(ByteReader& reader, Tick now) {
    u32 schema = 0;
    u32 revision = 0;
    if (!reader.read_u32(schema) || !reader.read_u32(revision)) {
        return make_status(StatusCode::Truncated, "governor state header is truncated");
    }
    if (schema != kSchemaVersion) {
        return make_status(StatusCode::VersionMismatch, "governor state schema is not supported");
    }
    if (revision != 1) {
        return make_status(StatusCode::VersionMismatch,
                           "governor state serialization revision is not supported");
    }

    u64 stored_last_tick = 0;
    u64 stored_sequence = 0;
    u64 stored_intent_sequence = 0;
    u64 stored_boot = 0;
    u64 stored_epoch = 0;
    u64 stored_started = 0;
    u64 stored_seed = 0;
    if (!reader.read_u64(stored_last_tick) || !reader.read_u64(stored_sequence) ||
        !reader.read_u64(stored_intent_sequence) || !reader.read_u64(stored_boot) ||
        !reader.read_u64(stored_epoch) || !reader.read_u64(stored_started) ||
        !reader.read_u64(stored_seed)) {
        return make_status(StatusCode::Truncated, "governor state scalars are truncated");
    }
    if (!incarnation_.boot.valid()) {
        return make_status(StatusCode::InvalidArgument,
                           "a fresh boot identity must be installed before restoring state");
    }
    if (incarnation_.boot.boot == BootId{stored_boot}) {
        return make_status(StatusCode::Conflict,
                           "restoring state requires a boot identity distinct from the stored one");
    }
    if (incarnation_.boot.epoch <= EpochId{stored_epoch}) {
        return make_status(StatusCode::EpochMismatch,
                           "restoring state requires an epoch beyond the stored one");
    }

    bool has_policy = false;
    if (!reader.read_bool(has_policy)) {
        return make_status(StatusCode::Truncated, "policy presence flag is truncated");
    }
    policy_.reset();
    exit_rules_ = RuleSet{};
    if (has_policy) {
        PolicyDocument policy;
        EFG_TRY(decode(reader, policy));
        EFG_TRY(policy.validate());
        StatusOr<RuleSet> exit_rules = effective_exit_rule(policy);
        if (!exit_rules.ok()) {
            return exit_rules.status();
        }
        policy_ = policy;
        exit_rules_ = std::move(exit_rules.value());
    }

    u32 capacity_count = 0;
    if (!reader.read_u32(capacity_count)) {
        return make_status(StatusCode::Truncated, "capacity count is truncated");
    }
    if (capacity_count > limits::kMaxResources) {
        return make_status(StatusCode::Oversized, "restored capacity population exceeds the ceiling");
    }
    capacities_ = CapacityTable{};
    for (u32 i = 0; i < capacity_count; ++i) {
        ResourceCapacity capacity;
        EFG_TRY(decode(reader, capacity));
        EFG_TRY(capacities_.submit(capacity));
    }

    u32 path_count = 0;
    if (!reader.read_u32(path_count)) {
        return make_status(StatusCode::Truncated, "path count is truncated");
    }
    if (path_count > limits::kMaxPaths) {
        return make_status(StatusCode::Oversized, "restored path population exceeds the ceiling");
    }
    paths_ = PathTable{};
    for (u32 i = 0; i < path_count; ++i) {
        PathDescriptor path;
        EFG_TRY(decode(reader, path));
        EFG_TRY(paths_.submit(path));
    }

    u32 flow_count = 0;
    if (!reader.read_u32(flow_count)) {
        return make_status(StatusCode::Truncated, "flow count is truncated");
    }
    if (static_cast<std::size_t>(flow_count) > config_.max_flows) {
        return make_status(StatusCode::Oversized,
                           "restored flow population exceeds the configured ceiling");
    }

    flows_.clear();
    highest_generation_.clear();
    binding_index_.clear();
    resource_aggregate_.clear();

    for (u32 i = 0; i < flow_count; ++i) {
        u64 flow_raw = 0;
        u64 generation_raw = 0;
        if (!reader.read_u64(flow_raw) || !reader.read_u64(generation_raw)) {
            return make_status(StatusCode::Truncated, "restored flow identity is truncated");
        }
        const FlowId flow{flow_raw};
        const Generation generation{generation_raw};
        if (!flow.valid() || !generation.valid()) {
            return make_status(StatusCode::Corrupt, "restored flow identity is invalid");
        }
        bool created = false;
        FlowRecord& record = create_flow(flow, generation, created);
        (void)created;

        bool has_classification = false;
        if (!reader.read_bool(has_classification)) {
            return make_status(StatusCode::Truncated, "restored classification flag is truncated");
        }
        FlowClassification classification;
        if (has_classification) {
            EFG_TRY(decode(reader, classification));
        }
        u64 evaluated_window = 0;
        if (!reader.read_u32(record.enter_streak) || !reader.read_u32(record.exit_streak) ||
            !reader.read_u64(record.evaluated_tick) || !reader.read_u64(evaluated_window)) {
            return make_status(StatusCode::Truncated, "restored flow scalar state is truncated");
        }
        record.evaluated_window = EvidenceWindowId{evaluated_window};
        u64 completed_tick = 0;
        bool ledger_completed = false;
        u64 ledger_completed_at = 0;
        bool gap_detected = false;
        if (!reader.read_u64(completed_tick) || !reader.read_bool(ledger_completed) ||
            !reader.read_u64(ledger_completed_at) || !reader.read_bool(gap_detected)) {
            return make_status(StatusCode::Truncated, "restored completion state is truncated");
        }
        record.completed_tick = completed_tick;
        if (gap_detected) {
            // Gap flags are re-derived from the replayed windows below.
        }

        u32 sample_count = 0;
        if (!reader.read_u32(sample_count)) {
            return make_status(StatusCode::Truncated, "restored sample count is truncated");
        }
        if (sample_count > limits::kMaxSamplesPerFlow) {
            return make_status(StatusCode::Oversized, "restored sample population exceeds the ceiling");
        }
        for (u32 s = 0; s < sample_count; ++s) {
            FlowSample sample;
            EFG_TRY(decode(reader, sample));
            StatusOr<AppendOutcome> appended = record.ledger.append(sample);
            if (!appended.ok()) {
                return make_status(StatusCode::Corrupt,
                                   "restored evidence window does not satisfy ledger invariants");
            }
        }
        if (ledger_completed) {
            EFG_TRY(record.ledger.mark_completed(ledger_completed_at));
        }

        u32 transition_count = 0;
        if (!reader.read_u32(transition_count)) {
            return make_status(StatusCode::Truncated, "restored transition count is truncated");
        }
        if (transition_count > limits::kMaxHistoryPerFlow) {
            return make_status(StatusCode::Oversized,
                               "restored transition population exceeds the ceiling");
        }
        for (u32 t = 0; t < transition_count; ++t) {
            ClassificationTransition transition;
            EFG_TRY(decode(reader, transition));
            EFG_TRY(record.history.record(transition));
        }
        u64 stored_digest = 0;
        u64 folded = 0;
        if (!reader.read_u64(stored_digest) || !reader.read_u64(folded)) {
            return make_status(StatusCode::Truncated, "restored history digest is truncated");
        }
        if (folded != 0) {
            return make_status(StatusCode::Unsupported,
                               "restored history with folded transitions is not supported");
        }
        if (stored_digest != record.history.digest()) {
            return make_status(StatusCode::IntegrityFailure,
                               "restored classification history digest does not match");
        }
        if (has_classification) {
            record.classification = classification;
            record.has_classification = true;
        }
    }

    u32 intent_count = 0;
    if (!reader.read_u32(intent_count)) {
        return make_status(StatusCode::Truncated, "intent count is truncated");
    }
    if (static_cast<std::size_t>(intent_count) > config_.max_intents) {
        return make_status(StatusCode::Oversized, "restored intent population exceeds the ceiling");
    }
    intents_ = IntentLedger(IntentLedger::Limits{config_.max_intents, limits::kMaxAttempts});
    for (u32 i = 0; i < intent_count; ++i) {
        GovernanceIntent intent;
        EFG_TRY(decode(reader, intent));
        StatusOr<GovernanceIntent> stored = intents_.record(intent);
        if (!stored.ok()) {
            return stored.status();
        }
    }

    classification_sequence_ = stored_sequence;
    intent_sequence_ = stored_intent_sequence;
    last_tick_ = stored_last_tick > now ? stored_last_tick : now;

    ++counters_.restore_events;
    return resume_after_restore(now);
}

Status Governor::resume_after_restore(Tick now) {
    // Nothing about liveness, telemetry freshness, publisher authority or
    // hardware effect survives a restart. Every restored classification is
    // demoted to Suspended and every live intent is fenced.
    std::size_t needing_revalidation = 0;
    std::size_t restored = 0;
    for (auto& entry : flows_) {
        FlowRecord& record = entry.second;
        ++restored;
        if (!record.has_classification) {
            continue;
        }
        const ElephantState from = record.classification.state;
        if (from == ElephantState::Elephant) {
            ++needing_revalidation;
        }
        ClassificationTransition transition;
        transition.at = now;
        transition.from = from;
        transition.to = ElephantState::Suspended;
        transition.authority = AuthorityLevel::None;
        transition.reasons = record.classification.reasons | ReasonCode::RevalidationRequired;
        transition.classification = record.classification.id;
        transition.classification_generation = record.classification.generation;
        EFG_TRY(record.history.record(transition));
        record.classification.state = ElephantState::Suspended;
        record.classification.authority = AuthorityLevel::None;
        record.classification.reasons |= ReasonCode::RevalidationRequired;
        record.classification.authority_vector.boot = incarnation_.boot.boot;
        record.classification.authority_vector.epoch = incarnation_.boot.epoch;
        record.enter_streak = 0;
        record.exit_streak = 0;
        record.evaluated_tick = now;
    }

    binding_index_.clear();
    resource_aggregate_.clear();
    for (auto& entry : flows_) {
        update_index(entry.first, entry.second);
    }

    u64 fenced = 0;
    EFG_TRY(intents_.revoke_all(now, IntentState::Fenced, SuppressReason::AuthorityStale, fenced));
    counters_.intents_fenced += fenced;

    counters_.restored_flows += restored;
    counters_.restored_flows_requiring_revalidation += needing_revalidation;

    tick_guard_.reset();
    EFG_TRY(tick_guard_.observe(now));
    last_tick_ = last_tick_ > now ? last_tick_ : now;
    return {};
}

StatusOr<u64> decision_digest(const Governor& governor) {
    Hasher hasher;
    hasher.write(version_string());
    const PolicyDocument* policy = governor.policy();
    hasher.write_bool(policy != nullptr);
    if (policy != nullptr) {
        hasher.write_digest(policy->digest);
    }
    for (const ResourceId resource : governor.capacities().resources()) {
        const ResourceCapacity* capacity = governor.capacities().find(resource);
        if (capacity != nullptr) {
            hasher.write_u64(capacity->digest());
        }
    }
    for (const PathId path : governor.paths().paths()) {
        const PathDescriptor* descriptor = governor.paths().find(path);
        if (descriptor != nullptr) {
            hasher.write_u64(descriptor->digest());
        }
    }
    for (const auto& key : governor.flow_keys()) {
        const FlowLedger* ledger = governor.ledger(key.first, key.second);
        if (ledger == nullptr) {
            continue;
        }
        hasher.write_u64(key.first.value());
        hasher.write_u64(key.second.value());
        const FlowMeasurements measurements = ledger->measurements();
        hasher.write_digest(measurements.digest());
        const ClassificationHistory* history = governor.history(key.first, key.second);
        if (history != nullptr) {
            hasher.write_digest(history->digest());
        }
    }
    for (const GovernanceIntent& intent : governor.intents().live_intents()) {
        hasher.write_u64(intent.id.value());
        hasher.write_u64(intent.digest);
    }
    const GovernorCounters& counters = governor.counters();
    hasher.write_u64(counters.decisions);
    hasher.write_u64(counters.transitions);
    return hasher.finish();
}

}  // namespace efg
