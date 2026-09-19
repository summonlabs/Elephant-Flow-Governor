// Elephant Flow Governor - bounded governance intent.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// An intent is a bounded, generation-bound, expiring statement of what an
// adjacent system is authorized to do. The governor never executes it. Every
// intent carries the authority vector that justified it, a requested envelope, a
// granted envelope that is never wider than the policy allows, and a validity
// window. Withdrawing authority revokes or fences the intent by identity, not by
// hope.

#ifndef EFG_INTERVENTION_HPP
#define EFG_INTERVENTION_HPP

#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include "efg/checked.hpp"
#include "efg/classification.hpp"
#include "efg/hash.hpp"
#include "efg/identity.hpp"
#include "efg/intent_kind.hpp"
#include "efg/limits.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"

namespace efg {

enum class IntentState : std::uint8_t {
    /// Produced but not authorized because the policy forbids it.
    Suppressed = 0,
    /// Authorized within the granted envelope and not yet consumed.
    Authorized = 1,
    /// Authorized, but the requested envelope exceeded the policy bound and was
    /// narrowed. The narrowed value is what appears in the granted envelope.
    Clamped = 2,
    /// The adjacent system acknowledged consumption. Purely an accounting state.
    Active = 3,
    /// Authority withdrawn normally: the classification changed.
    Revoked = 4,
    /// Authority withdrawn because a generation, boot or epoch moved. A fenced
    /// intent must not be honored even if an actuator never saw the revocation.
    Fenced = 5,
    /// The intent's validity window closed.
    Expired = 6,
    /// The flow reported a clean completion.
    Completed = 7,
};

[[nodiscard]] std::string_view to_string(IntentState state) noexcept;

/// True when the intent still carries authority that an actuator may act on.
[[nodiscard]] constexpr bool is_live(IntentState state) noexcept {
    return state == IntentState::Authorized || state == IntentState::Clamped ||
           state == IntentState::Active;
}

enum class SuppressReason : std::uint32_t {
    None = 0,
    ProtectedObligation = 1u << 0,
    NonPreemptibleObligation = 1u << 1,
    ReservationObligation = 1u << 2,
    PolicyForbids = 1u << 3,
    AuthorityStale = 1u << 4,
    NoAuthority = 1u << 5,
    ImpactBelowThreshold = 1u << 6,
    ContentionBelowThreshold = 1u << 7,
    BoundsExceeded = 1u << 8,
    FlowCompleted = 1u << 9,
    CapacityUnknown = 1u << 10,
    LedgerBudgetExhausted = 1u << 11,
    ShuttingDown = 1u << 12,
};

[[nodiscard]] std::string_view to_string(SuppressReason reason) noexcept;

/// The bounded envelope of an intent. Nothing here is advisory: a granted
/// envelope is the contract.
struct IntentBounds {
    /// Maximum fraction of the flow's sustained rate that may be removed.
    BasisPoints rate_reduction_bp{0};
    /// Maximum lifetime in logical ticks.
    u64 duration_ticks{0};
    /// Maximum number of named targets.
    u32 targets{0};

    bool placement{false};
    bool isolation{false};
    bool admission_reduction{false};
    bool congestion_escalation{false};

    friend constexpr bool operator==(const IntentBounds&, const IntentBounds&) noexcept = default;
};

/// Element-wise narrowing of a requested envelope to a ceiling envelope.
[[nodiscard]] IntentBounds narrow_bounds(const IntentBounds& requested,
                                         const IntentBounds& ceiling) noexcept;

/// True when the requested envelope is already inside the ceiling.
[[nodiscard]] bool bounds_within(const IntentBounds& requested,
                                 const IntentBounds& ceiling) noexcept;

struct GovernanceIntent {
    IntentId id{};
    AttemptId attempt{};
    IntentKind kind{IntentKind::ObserveOnly};
    FlowId flow{};
    Generation flow_generation{};
    ClassificationId classification{};
    Generation classification_generation{};
    IntentState state{IntentState::Authorized};
    SuppressReason suppressed{SuppressReason::None};
    IntentBounds requested{};
    IntentBounds granted{};
    BasisPoints severity{0};
    BasisPoints contention_pressure{0};
    u32 competing_elephants{0};
    TickSpan window{};
    AuthorityVector authority{};
    Provenance provenance{};
    u64 digest{0};

    [[nodiscard]] bool live_at(Tick now) const noexcept {
        return is_live(state) && now >= window.begin && now < window.end;
    }

    [[nodiscard]] u64 compute_digest() const;
};

void encode(ByteWriter& writer, const GovernanceIntent& intent);

/// Bounded ledger of governance intent with idempotent attempt recording.
///
/// Attempt identifiers make production idempotent: re-recording an identical
/// attempt returns the stored intent unchanged; re-using an attempt identifier
/// with a different payload is a conflict and is refused.
class IntentLedger {
public:
    struct Limits {
        std::size_t max_intents{limits::kMaxIntents};
        std::size_t max_attempts{limits::kMaxAttempts};
    };

    struct Counters {
        u64 recorded{0};
        u64 duplicates{0};
        u64 conflicts{0};
        u64 revocations{0};
        u64 fences{0};
        u64 expirations{0};
        u64 completions{0};
        u64 rejected_budget{0};
    };

    IntentLedger() = default;
    explicit IntentLedger(Limits limits) : limits_(limits) {}

    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }

    /// Record an intent. Idempotent on AttemptId.
    [[nodiscard]] StatusOr<GovernanceIntent> record(const GovernanceIntent& intent);

    /// Change the state of a stored intent. Returns NotFound when absent.
    Status transition(IntentId id, IntentState state, SuppressReason reason, Tick now);

    /// Revoke every live intent for one flow generation.
    Status revoke_flow(FlowId flow, Generation generation, Tick now, u64& revoked);

    /// Fence every live intent bound to generations other than the supplied one.
    Status fence_other_generations(FlowId flow, Generation keep_generation, Tick now, u64& fenced);

    /// Fence every live intent from an older epoch.
    Status fence_epoch(EpochId current_epoch, Tick now, u64& fenced);

    /// Fence every live intent from a different boot incarnation.
    Status fence_boot(BootId current_boot, Tick now, u64& fenced);

    /// Transition every live intent in one pass. Used when a policy generation
    /// change or a restart invalidates the whole decision surface at once.
    Status revoke_all(Tick now, IntentState state, SuppressReason reason, u64& changed);

    /// Revoke every live intent for one flow generation whose authorizing
    /// classification generation is no longer current.
    Status revoke_stale_generation(FlowId flow, Generation generation, Generation keep, Tick now,
                                   u64& changed);

    /// Expire every live intent whose window closed at or before now.
    Status expire(Tick now, u64& expired);

    Status transition_flow_terminal(FlowId flow, Generation generation, IntentState state, Tick now,
                                    u64& changed);

    [[nodiscard]] const GovernanceIntent* find(IntentId id) const;
    [[nodiscard]] const GovernanceIntent* find_attempt(AttemptId attempt) const;
    [[nodiscard]] std::vector<GovernanceIntent> for_flow(FlowId flow, Generation generation) const;
    [[nodiscard]] std::vector<GovernanceIntent> live_intents() const;
    /// Every recorded intent in ascending identifier order.
    [[nodiscard]] std::vector<GovernanceIntent> all_intents() const;
    [[nodiscard]] std::size_t size() const noexcept { return intents_.size(); }
    [[nodiscard]] std::size_t live_count() const noexcept;

    void clear() noexcept;

private:
    [[nodiscard]] Status ensure_capacity();

    Limits limits_{};
    std::map<IntentId, GovernanceIntent> intents_;
    std::map<AttemptId, IntentId> attempts_;
    IntentId next_id_{};
    Counters counters_{};
};

}  // namespace efg

#endif  // EFG_INTERVENTION_HPP
