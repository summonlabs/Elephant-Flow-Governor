// Elephant Flow Governor - explanation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Explanation is a first class output, not a debug aid. It states why a flow
// qualified, which thresholds were evaluated and with what observed values, how
// long and how fast the flow is, what share of which resource it occupies, what
// protection applies, which interventions are authorized and which are
// suppressed, and the exact authority vector the answer rests on.

#ifndef EFG_EXPLAIN_HPP
#define EFG_EXPLAIN_HPP

#include <string>
#include <string_view>
#include <vector>

#include "efg/classification.hpp"
#include "efg/intervention.hpp"
#include "efg/limits.hpp"
#include "efg/status.hpp"

namespace efg {

enum class ExplanationKind : std::uint8_t {
    Qualification = 0,
    Disqualification = 1,
    Indeterminate = 2,
    Suspension = 3,
    Protection = 4,
    Authority = 5,
    Impact = 6,
    Intent = 7,
    Suppression = 8,
    History = 9,
};

[[nodiscard]] std::string_view to_string(ExplanationKind kind) noexcept;

struct ExplanationLine {
    ExplanationKind kind{ExplanationKind::Qualification};
    std::string text{};
};

struct Explanation {
    FlowId flow{};
    Generation flow_generation{};
    bool found{false};

    ElephantState state{ElephantState::Unknown};
    AuthorityLevel authority{AuthorityLevel::None};
    ReasonCode reasons{ReasonCode::None};

    std::string summary{};
    std::vector<ExplanationLine> lines{};
    bool truncated{false};

    FlowMeasurements measurements{};
    ImpactAssessment impact{};
    std::vector<ThresholdTraceEntry> trace{};
    AuthorityVector authority_vector{};
    std::vector<GovernanceIntent> intents{};
    u64 history_digest{0};
    u64 history_folded{0};

    [[nodiscard]] u64 digest() const;
};

/// Build the bounded, ordered explanation lines for a decision. The lines are
/// produced from the structured fields only, so the rendered explanation can
/// never disagree with the record it describes.
[[nodiscard]] std::vector<ExplanationLine> build_explanation_lines(const Explanation& explanation,
                                                                  bool& truncated);

/// Render a bounded, stable textual explanation.
[[nodiscard]] std::string render_text(const Explanation& explanation);

/// Render a bounded, stable JSON explanation. The JSON is hand written so that
/// key order and number formatting are deterministic and no locale is consulted.
[[nodiscard]] std::string render_json(const Explanation& explanation);

/// Render the governor's net authority position as a compact vector string.
[[nodiscard]] std::string render_authority_vector(const AuthorityVector& authority);

}  // namespace efg

#endif  // EFG_EXPLAIN_HPP
