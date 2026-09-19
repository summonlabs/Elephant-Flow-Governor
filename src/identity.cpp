// Elephant Flow Governor - identity rendering.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/identity.hpp"

namespace efg {

std::string_view to_string(BindingVerdict verdict) noexcept {
    switch (verdict) {
        case BindingVerdict::Match: return "match";
        case BindingVerdict::FlowGenerationChanged: return "flow_generation_changed";
        case BindingVerdict::PathGenerationChanged: return "path_generation_changed";
        case BindingVerdict::CapacityGenerationChanged: return "capacity_generation_changed";
        case BindingVerdict::PolicyGenerationChanged: return "policy_generation_changed";
        case BindingVerdict::EvidenceWindowSuperseded: return "evidence_window_superseded";
        case BindingVerdict::EpochChanged: return "epoch_changed";
        case BindingVerdict::BootChanged: return "boot_changed";
        case BindingVerdict::PublisherChanged: return "publisher_changed";
        case BindingVerdict::PublisherIncarnationChanged: return "publisher_incarnation_changed";
        case BindingVerdict::Absent: return "absent";
    }
    return "absent";
}

}  // namespace efg
