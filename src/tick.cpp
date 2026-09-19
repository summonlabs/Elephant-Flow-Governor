// Elephant Flow Governor - logical time rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/tick.hpp"

namespace efg {

std::string_view to_string(FreshnessVerdict verdict) noexcept {
    switch (verdict) {
        case FreshnessVerdict::Fresh: return "fresh";
        case FreshnessVerdict::NotYetValid: return "not_yet_valid";
        case FreshnessVerdict::Expired: return "expired";
        case FreshnessVerdict::Malformed: return "malformed";
        case FreshnessVerdict::Regressed: return "regressed";
    }
    return "malformed";
}

}  // namespace efg
