// Elephant Flow Governor - governance intent vocabulary.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// These are bounded *intents*, not enforcement actions. Elephant Flow Governor
// never shapes, schedules, admits, reserves, paces or forwards anything; it
// states which bounded corrective intent is authorized, against which exact
// generations, and until when. Execution belongs to an adjacent system.

#ifndef EFG_INTENT_KIND_HPP
#define EFG_INTENT_KIND_HPP

#include <cstdint>
#include <string_view>

namespace efg {

enum class IntentKind : std::uint8_t {
    /// No corrective action. Emitted when classification holds but no bounded
    /// action is authorized, so that "classified and deliberately untouched" is
    /// distinguishable from "not classified".
    ObserveOnly = 0,

    /// Request that the flow be considered for a different path.
    RequestAlternatePlacement = 1,

    /// Request that the flow's rate be reduced by at most the granted portion.
    RequestRateShaping = 2,

    /// Request scheduling isolation so that the flow stops perturbing others.
    RequestSchedulingIsolation = 3,

    /// Directive to keep a protected or reserved obligation intact.
    ProtectReservedFlow = 4,

    /// Request that competing admission to the binding resource be reduced.
    ReduceCompetingAdmission = 5,

    /// Escalate to congestion control because the resource is over-subscribed.
    EscalateCongestion = 6,

    Count = 7,
};

[[nodiscard]] std::string_view to_string(IntentKind kind) noexcept;
[[nodiscard]] bool parse_intent_kind(std::string_view text, IntentKind& out) noexcept;

/// True when the intent asks an adjacent system to change something. ObserveOnly
/// and ProtectReservedFlow are declarative and change nothing.
[[nodiscard]] constexpr bool is_corrective(IntentKind kind) noexcept {
    return kind != IntentKind::ObserveOnly && kind != IntentKind::ProtectReservedFlow;
}

/// True when the intent is a protection directive rather than a corrective one.
[[nodiscard]] constexpr bool is_protective(IntentKind kind) noexcept {
    return kind == IntentKind::ProtectReservedFlow;
}

}  // namespace efg

#endif  // EFG_INTENT_KIND_HPP
