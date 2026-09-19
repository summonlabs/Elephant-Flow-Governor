// Elephant Flow Governor - bounded governance intent implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/intervention.hpp"

#include <algorithm>

namespace efg {

std::string_view to_string(IntentKind kind) noexcept {
    switch (kind) {
        case IntentKind::ObserveOnly: return "observe_only";
        case IntentKind::RequestAlternatePlacement: return "request_alternate_placement";
        case IntentKind::RequestRateShaping: return "request_rate_shaping";
        case IntentKind::RequestSchedulingIsolation: return "request_scheduling_isolation";
        case IntentKind::ProtectReservedFlow: return "protect_reserved_flow";
        case IntentKind::ReduceCompetingAdmission: return "reduce_competing_admission";
        case IntentKind::EscalateCongestion: return "escalate_congestion";
        case IntentKind::Count: return "count";
    }
    return "observe_only";
}

bool parse_intent_kind(std::string_view text, IntentKind& out) noexcept {
    for (u8 i = 0; i < static_cast<u8>(IntentKind::Count); ++i) {
        const auto kind = static_cast<IntentKind>(i);
        if (to_string(kind) == text) {
            out = kind;
            return true;
        }
    }
    return false;
}

std::string_view to_string(IntentState state) noexcept {
    switch (state) {
        case IntentState::Suppressed: return "suppressed";
        case IntentState::Authorized: return "authorized";
        case IntentState::Clamped: return "clamped";
        case IntentState::Active: return "active";
        case IntentState::Revoked: return "revoked";
        case IntentState::Fenced: return "fenced";
        case IntentState::Expired: return "expired";
        case IntentState::Completed: return "completed";
    }
    return "suppressed";
}

std::string_view to_string(SuppressReason reason) noexcept {
    switch (reason) {
        case SuppressReason::None: return "none";
        case SuppressReason::ProtectedObligation: return "protected_obligation";
        case SuppressReason::NonPreemptibleObligation: return "non_preemptible_obligation";
        case SuppressReason::ReservationObligation: return "reservation_obligation";
        case SuppressReason::PolicyForbids: return "policy_forbids";
        case SuppressReason::AuthorityStale: return "authority_stale";
        case SuppressReason::NoAuthority: return "no_authority";
        case SuppressReason::ImpactBelowThreshold: return "impact_below_threshold";
        case SuppressReason::ContentionBelowThreshold: return "contention_below_threshold";
        case SuppressReason::BoundsExceeded: return "bounds_exceeded";
        case SuppressReason::FlowCompleted: return "flow_completed";
        case SuppressReason::CapacityUnknown: return "capacity_unknown";
        case SuppressReason::LedgerBudgetExhausted: return "ledger_budget_exhausted";
        case SuppressReason::ShuttingDown: return "shutting_down";
    }
    return "none";
}

IntentBounds narrow_bounds(const IntentBounds& requested, const IntentBounds& ceiling) noexcept {
    IntentBounds out;
    out.rate_reduction_bp = requested.rate_reduction_bp < ceiling.rate_reduction_bp
                                ? requested.rate_reduction_bp
                                : ceiling.rate_reduction_bp;
    out.duration_ticks = min_of(requested.duration_ticks, ceiling.duration_ticks);
    out.targets = requested.targets < ceiling.targets ? requested.targets : ceiling.targets;
    out.placement = requested.placement && ceiling.placement;
    out.isolation = requested.isolation && ceiling.isolation;
    out.admission_reduction = requested.admission_reduction && ceiling.admission_reduction;
    out.congestion_escalation = requested.congestion_escalation && ceiling.congestion_escalation;
    return out;
}

bool bounds_within(const IntentBounds& requested, const IntentBounds& ceiling) noexcept {
    const IntentBounds narrowed = narrow_bounds(requested, ceiling);
    return narrowed == requested;
}

u64 GovernanceIntent::compute_digest() const {
    Hasher hasher;
    hasher.write_u64(id.value());
    hasher.write_u64(attempt.value());
    hasher.write_u8(static_cast<u8>(kind));
    hasher.write_u64(flow.value());
    hasher.write_u64(flow_generation.value());
    hasher.write_u64(classification.value());
    hasher.write_u64(classification_generation.value());
    hasher.write_u8(static_cast<u8>(state));
    hasher.write_u32(static_cast<u32>(suppressed));
    hasher.write_u32(requested.rate_reduction_bp);
    hasher.write_u64(requested.duration_ticks);
    hasher.write_u32(requested.targets);
    hasher.write_bool(requested.placement);
    hasher.write_bool(requested.isolation);
    hasher.write_bool(requested.admission_reduction);
    hasher.write_bool(requested.congestion_escalation);
    hasher.write_u32(granted.rate_reduction_bp);
    hasher.write_u64(granted.duration_ticks);
    hasher.write_u32(granted.targets);
    hasher.write_bool(granted.placement);
    hasher.write_bool(granted.isolation);
    hasher.write_bool(granted.admission_reduction);
    hasher.write_bool(granted.congestion_escalation);
    hasher.write_u32(severity);
    hasher.write_u32(contention_pressure);
    hasher.write_u32(competing_elephants);
    hasher.write_u64(window.begin);
    hasher.write_u64(window.end);
    hasher.write_digest(authority.digest());
    return hasher.finish();
}

Status IntentLedger::ensure_capacity() {
    if (intents_.size() >= limits_.max_intents || attempts_.size() >= limits_.max_attempts) {
        ++counters_.rejected_budget;
        return make_status(StatusCode::CapacityExceeded, "intent ledger budget is exhausted");
    }
    return {};
}

StatusOr<GovernanceIntent> IntentLedger::record(const GovernanceIntent& intent) {
    if (!intent.attempt.valid()) {
        return make_status(StatusCode::InvalidArgument, "intent attempt identifier is absent");
    }
    if (!intent.flow.valid() || !intent.flow_generation.valid()) {
        return make_status(StatusCode::InvalidArgument, "intent flow binding is incomplete");
    }
    if (intent.window.end <= intent.window.begin) {
        return make_status(StatusCode::InvalidArgument, "intent window is empty or inverted");
    }
    if (intent.kind >= IntentKind::Count) {
        return make_status(StatusCode::InvalidArgument, "intent kind is not recognized");
    }
    switch (intent.state) {
        case IntentState::Suppressed:
        case IntentState::Authorized:
        case IntentState::Clamped:
        case IntentState::Active:
        case IntentState::Revoked:
        case IntentState::Fenced:
        case IntentState::Expired:
        case IntentState::Completed:
            break;
        default:
            return make_status(StatusCode::InvalidArgument, "intent state is not recognized");
    }

    const auto existing = attempts_.find(intent.attempt);
    if (existing != attempts_.end()) {
        const auto stored = intents_.find(existing->second);
        if (stored == intents_.end()) {
            return make_status(StatusCode::Internal, "attempt index references a missing intent");
        }
        GovernanceIntent candidate = intent;
        candidate.id = stored->second.id;
        candidate.digest = candidate.compute_digest();
        if (candidate.compute_digest() == stored->second.digest) {
            ++counters_.duplicates;
            return stored->second;
        }
        ++counters_.conflicts;
        return make_status(StatusCode::Conflict,
                           "attempt identifier reused with different intent content");
    }

    EFG_TRY(ensure_capacity());

    GovernanceIntent stored = intent;
    if (!stored.id.valid()) {
        if (next_id_.value() == UINT64_MAX) {
            return make_status(StatusCode::Overflow, "intent identifier space is exhausted");
        }
        next_id_ = IntentId{next_id_.value() + 1};
        stored.id = next_id_;
    } else if (intents_.count(stored.id) != 0) {
        return make_status(StatusCode::AlreadyExists, "intent identifier is already in use");
    } else if (stored.id.value() >= next_id_.value()) {
        if (stored.id.value() == UINT64_MAX) {
            return make_status(StatusCode::Overflow, "intent identifier space is exhausted");
        }
        next_id_ = IntentId{stored.id.value() + 1};
    }
    stored.digest = stored.compute_digest();

    attempts_.emplace(stored.attempt, stored.id);
    intents_.emplace(stored.id, stored);
    ++counters_.recorded;
    return stored;
}

Status IntentLedger::transition(IntentId id, IntentState state, SuppressReason reason, Tick now) {
    const auto it = intents_.find(id);
    if (it == intents_.end()) {
        return make_status(StatusCode::NotFound, "intent identifier is not present in the ledger");
    }
    GovernanceIntent& intent = it->second;
    if (intent.state == state && intent.suppressed == reason) {
        return {};
    }
    switch (state) {
        case IntentState::Revoked: ++counters_.revocations; break;
        case IntentState::Fenced: ++counters_.fences; break;
        case IntentState::Expired: ++counters_.expirations; break;
        case IntentState::Completed: ++counters_.completions; break;
        default: break;
    }
    intent.state = state;
    intent.suppressed = reason;
    if (state != IntentState::Authorized && state != IntentState::Clamped &&
        state != IntentState::Active && now > intent.window.end) {
        intent.window.end = now;
    }
    intent.digest = intent.compute_digest();
    return {};
}

Status IntentLedger::transition_flow_terminal(FlowId flow, Generation generation,
                                              IntentState state, Tick now, u64& changed) {
    changed = 0;
    for (auto& entry : intents_) {
        GovernanceIntent& intent = entry.second;
        if (!is_live(intent.state)) {
            continue;
        }
        if (intent.flow != flow || intent.flow_generation != generation) {
            continue;
        }
        intent.state = state;
        intent.digest = intent.compute_digest();
        ++changed;
        switch (state) {
            case IntentState::Revoked: ++counters_.revocations; break;
            case IntentState::Fenced: ++counters_.fences; break;
            case IntentState::Expired: ++counters_.expirations; break;
            case IntentState::Completed: ++counters_.completions; break;
            default: break;
        }
    }
    (void)now;
    return {};
}

Status IntentLedger::revoke_flow(FlowId flow, Generation generation, Tick now, u64& revoked) {
    return transition_flow_terminal(flow, generation, IntentState::Revoked, now, revoked);
}

Status IntentLedger::fence_other_generations(FlowId flow, Generation keep_generation, Tick now,
                                             u64& fenced) {
    fenced = 0;
    for (auto& entry : intents_) {
        GovernanceIntent& intent = entry.second;
        if (!is_live(intent.state)) {
            continue;
        }
        if (intent.flow != flow || intent.flow_generation == keep_generation) {
            continue;
        }
        intent.state = IntentState::Fenced;
        intent.suppressed = SuppressReason::AuthorityStale;
        intent.digest = intent.compute_digest();
        ++fenced;
        ++counters_.fences;
    }
    (void)now;
    return {};
}

Status IntentLedger::fence_epoch(EpochId current_epoch, Tick now, u64& fenced) {
    fenced = 0;
    for (auto& entry : intents_) {
        GovernanceIntent& intent = entry.second;
        if (!is_live(intent.state)) {
            continue;
        }
        if (intent.authority.epoch == current_epoch) {
            continue;
        }
        intent.state = IntentState::Fenced;
        intent.suppressed = SuppressReason::AuthorityStale;
        intent.digest = intent.compute_digest();
        ++fenced;
        ++counters_.fences;
    }
    (void)now;
    return {};
}

Status IntentLedger::fence_boot(BootId current_boot, Tick now, u64& fenced) {
    fenced = 0;
    for (auto& entry : intents_) {
        GovernanceIntent& intent = entry.second;
        if (!is_live(intent.state)) {
            continue;
        }
        if (intent.authority.boot == current_boot) {
            continue;
        }
        intent.state = IntentState::Fenced;
        intent.suppressed = SuppressReason::AuthorityStale;
        intent.digest = intent.compute_digest();
        ++fenced;
        ++counters_.fences;
    }
    (void)now;
    return {};
}

Status IntentLedger::revoke_all(Tick now, IntentState state, SuppressReason reason, u64& changed) {
    changed = 0;
    for (auto& entry : intents_) {
        GovernanceIntent& intent = entry.second;
        if (!is_live(intent.state)) {
            continue;
        }
        intent.state = state;
        intent.suppressed = reason;
        intent.digest = intent.compute_digest();
        ++changed;
        switch (state) {
            case IntentState::Revoked: ++counters_.revocations; break;
            case IntentState::Fenced: ++counters_.fences; break;
            case IntentState::Expired: ++counters_.expirations; break;
            case IntentState::Completed: ++counters_.completions; break;
            default: break;
        }
    }
    (void)now;
    return {};
}

Status IntentLedger::revoke_stale_generation(FlowId flow, Generation generation, Generation keep,
                                              Tick now, u64& changed) {
    changed = 0;
    for (auto& entry : intents_) {
        GovernanceIntent& intent = entry.second;
        if (!is_live(intent.state)) {
            continue;
        }
        if (intent.flow != flow || intent.flow_generation != generation) {
            continue;
        }
        if (intent.classification_generation == keep) {
            continue;
        }
        intent.state = IntentState::Revoked;
        intent.suppressed = SuppressReason::AuthorityStale;
        intent.digest = intent.compute_digest();
        ++changed;
        ++counters_.revocations;
    }
    (void)now;
    return {};
}

Status IntentLedger::expire(Tick now, u64& expired) {
    expired = 0;
    for (auto& entry : intents_) {
        GovernanceIntent& intent = entry.second;
        if (!is_live(intent.state)) {
            continue;
        }
        if (now < intent.window.end) {
            continue;
        }
        intent.state = IntentState::Expired;
        intent.digest = intent.compute_digest();
        ++expired;
        ++counters_.expirations;
    }
    return {};
}

const GovernanceIntent* IntentLedger::find(IntentId id) const {
    const auto it = intents_.find(id);
    return it == intents_.end() ? nullptr : &it->second;
}

const GovernanceIntent* IntentLedger::find_attempt(AttemptId attempt) const {
    const auto it = attempts_.find(attempt);
    if (it == attempts_.end()) {
        return nullptr;
    }
    return find(it->second);
}

std::vector<GovernanceIntent> IntentLedger::for_flow(FlowId flow, Generation generation) const {
    std::vector<GovernanceIntent> out;
    for (const auto& entry : intents_) {
        if (entry.second.flow == flow && entry.second.flow_generation == generation) {
            if (out.size() >= limits::kMaxIntentListPerFlow) {
                break;
            }
            out.push_back(entry.second);
        }
    }
    return out;
}

std::vector<GovernanceIntent> IntentLedger::live_intents() const {
    std::vector<GovernanceIntent> out;
    for (const auto& entry : intents_) {
        if (is_live(entry.second.state)) {
            out.push_back(entry.second);
        }
    }
    return out;
}

std::vector<GovernanceIntent> IntentLedger::all_intents() const {
    std::vector<GovernanceIntent> out;
    out.reserve(intents_.size());
    for (const auto& entry : intents_) {
        out.push_back(entry.second);
    }
    return out;
}

std::size_t IntentLedger::live_count() const noexcept {
    std::size_t count = 0;
    for (const auto& entry : intents_) {
        if (is_live(entry.second.state)) {
            ++count;
        }
    }
    return count;
}

void IntentLedger::clear() noexcept {
    intents_.clear();
    attempts_.clear();
    counters_ = Counters{};
}

}  // namespace efg
