// Elephant Flow Governor - three valued reasoning and reason codes.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Missing evidence is never treated as a negative answer. Predicates over flow
// measurements therefore evaluate to True, False or Unknown, combined with
// Kleene logic, and a composed rule that yields Unknown produces an Unknown
// classification with no governance authority at all.

#ifndef EFG_TRI_HPP
#define EFG_TRI_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace efg {

enum class Tri : std::uint8_t {
    False = 0,
    True = 1,
    Unknown = 2,
};

[[nodiscard]] constexpr Tri tri_not(Tri value) noexcept {
    switch (value) {
        case Tri::False:
            return Tri::True;
        case Tri::True:
            return Tri::False;
        case Tri::Unknown:
        default:
            return Tri::Unknown;
    }
}

[[nodiscard]] constexpr Tri tri_and(Tri lhs, Tri rhs) noexcept {
    if (lhs == Tri::False || rhs == Tri::False) {
        return Tri::False;
    }
    if (lhs == Tri::Unknown || rhs == Tri::Unknown) {
        return Tri::Unknown;
    }
    return Tri::True;
}

[[nodiscard]] constexpr Tri tri_or(Tri lhs, Tri rhs) noexcept {
    if (lhs == Tri::True || rhs == Tri::True) {
        return Tri::True;
    }
    if (lhs == Tri::Unknown || rhs == Tri::Unknown) {
        return Tri::Unknown;
    }
    return Tri::False;
}

[[nodiscard]] constexpr Tri tri_from_bool(bool value) noexcept {
    return value ? Tri::True : Tri::False;
}

[[nodiscard]] constexpr bool tri_is_true(Tri value) noexcept { return value == Tri::True; }
[[nodiscard]] constexpr bool tri_is_unknown(Tri value) noexcept { return value == Tri::Unknown; }

[[nodiscard]] std::string_view to_string(Tri value) noexcept;

/// Reason codes attached to classifications, impacts and intents. They are a
/// bitmask so that several independent justifications can be reported at once.
enum class ReasonCode : std::uint64_t {
    None = 0,

    // Qualification reasons (positive).
    VolumeThresholdMet = 1ull << 0,
    DurationThresholdMet = 1ull << 1,
    SustainedRateThresholdMet = 1ull << 2,
    ResourceShareThresholdMet = 1ull << 3,
    PolicyRuleSatisfied = 1ull << 4,
    HysteresisHeld = 1ull << 5,
    ExitThresholdNotReached = 1ull << 6,

    // Disqualification and refusal reasons (fail closed).
    BelowVolumeThreshold = 1ull << 8,
    BelowDurationThreshold = 1ull << 9,
    BelowRateThreshold = 1ull << 10,
    BelowShareThreshold = 1ull << 11,
    PolicyRuleNotSatisfied = 1ull << 12,
    EvidenceMissing = 1ull << 13,
    EvidenceStale = 1ull << 14,
    EvidenceUnknownField = 1ull << 15,
    TelemetryGap = 1ull << 16,
    CapacityMissing = 1ull << 17,
    CapacityStale = 1ull << 18,
    PathUnknown = 1ull << 19,
    PathGenerationChanged = 1ull << 20,
    FlowGenerationChanged = 1ull << 21,
    PolicyChanged = 1ull << 22,
    FlowCompleted = 1ull << 23,
    PolicyRuleIndeterminate = 1ull << 24,
    RevalidationRequired = 1ull << 25,

    // Protection reasons.
    ProtectedObligation = 1ull << 32,
    NonPreemptibleObligation = 1ull << 33,
    ReservedCapacityBounded = 1ull << 34,
    ControlClassBounded = 1ull << 35,

    // Governance reasons.
    GovernanceAuthorized = 1ull << 40,
    GovernanceSuppressed = 1ull << 41,
    GovernanceClampedToBounds = 1ull << 42,
    ImpactBelowActionThreshold = 1ull << 43,
    ContentionBelowActionThreshold = 1ull << 44,
};

[[nodiscard]] constexpr ReasonCode operator|(ReasonCode lhs, ReasonCode rhs) noexcept {
    return static_cast<ReasonCode>(static_cast<std::uint64_t>(lhs) |
                                   static_cast<std::uint64_t>(rhs));
}

[[nodiscard]] constexpr ReasonCode operator&(ReasonCode lhs, ReasonCode rhs) noexcept {
    return static_cast<ReasonCode>(static_cast<std::uint64_t>(lhs) &
                                   static_cast<std::uint64_t>(rhs));
}

constexpr ReasonCode& operator|=(ReasonCode& lhs, ReasonCode rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

[[nodiscard]] constexpr bool has_reason(ReasonCode set, ReasonCode flag) noexcept {
    return (static_cast<std::uint64_t>(set) & static_cast<std::uint64_t>(flag)) != 0;
}

[[nodiscard]] constexpr ReasonCode clear_reason(ReasonCode set, ReasonCode flag) noexcept {
    return static_cast<ReasonCode>(static_cast<std::uint64_t>(set) &
                                   ~static_cast<std::uint64_t>(flag));
}

/// Stable, sorted, comma separated rendering of the set bits. Bounded because
/// the code set is a closed enumeration.
[[nodiscard]] std::vector<std::string_view> reason_names(ReasonCode set);
[[nodiscard]] std::string render_reasons(ReasonCode set);

}  // namespace efg

#endif  // EFG_TRI_HPP
