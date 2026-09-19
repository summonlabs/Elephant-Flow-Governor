// Elephant Flow Governor - classification implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/classification.hpp"

#include <algorithm>

namespace efg {

std::string_view to_string(ElephantState state) noexcept {
    switch (state) {
        case ElephantState::Unknown: return "unknown";
        case ElephantState::NotElephant: return "not_elephant";
        case ElephantState::Elephant: return "elephant";
        case ElephantState::Suspended: return "suspended";
    }
    return "unknown";
}

std::string_view to_string(AuthorityLevel level) noexcept {
    switch (level) {
        case AuthorityLevel::None: return "none";
        case AuthorityLevel::Observe: return "observe";
        case AuthorityLevel::Govern: return "govern";
    }
    return "none";
}

BindingVerdict AuthorityVector::compare(const AuthorityVector& current) const {
    if (binds(kAuthorityFlowBound)) {
        if (current.flow != flow) {
            return BindingVerdict::Absent;
        }
        if (current.flow_generation != flow_generation) {
            return BindingVerdict::FlowGenerationChanged;
        }
    }
    if (binds(kAuthorityPathBound)) {
        // The generation is checked first: a re-route under a new generation is a
        // generation change even when the identifier is unchanged.
        if (current.path_generation != path_generation) {
            return BindingVerdict::PathGenerationChanged;
        }
        if (current.path != path) {
            return BindingVerdict::Absent;
        }
    }
    if (binds(kAuthorityCapacityBound)) {
        if (current.capacity_generation != capacity_generation) {
            return BindingVerdict::CapacityGenerationChanged;
        }
        if (current.capacity != capacity) {
            return BindingVerdict::Absent;
        }
    }
    if (binds(kAuthorityPolicyBound)) {
        if (current.policy_generation != policy_generation) {
            return BindingVerdict::PolicyGenerationChanged;
        }
        if (current.policy != policy) {
            return BindingVerdict::Absent;
        }
    }
    if (binds(kAuthorityWindowBound)) {
        // The recorded window is an audit record of the evidence the decision
        // used. A newer window supersedes it harmlessly; only a publisher whose
        // sequence moved backwards invalidates the binding.
        if (current.window_sequence < window_sequence) {
            return BindingVerdict::EvidenceWindowSuperseded;
        }
    }
    if (binds(kAuthorityEpochBound) && current.epoch != epoch) {
        return BindingVerdict::EpochChanged;
    }
    if (binds(kAuthorityBootBound) && current.boot != boot) {
        return BindingVerdict::BootChanged;
    }
    if (binds(kAuthorityPublisherBound)) {
        if (current.publisher != publisher) {
            return BindingVerdict::PublisherChanged;
        }
        if (current.publisher_boot != publisher_boot) {
            return BindingVerdict::PublisherIncarnationChanged;
        }
    }
    return BindingVerdict::Match;
}

u64 AuthorityVector::digest() const {
    Hasher hasher;
    hasher.write_u64(flow.value());
    hasher.write_u64(flow_generation.value());
    hasher.write_u64(path.value());
    hasher.write_u64(path_generation.value());
    hasher.write_u64(capacity.value());
    hasher.write_u64(capacity_generation.value());
    hasher.write_u64(window.value());
    hasher.write_u64(window_sequence);
    hasher.write_u64(policy.value());
    hasher.write_u64(policy_generation.value());
    hasher.write_u64(classification_generation.value());
    hasher.write_u64(epoch.value());
    hasher.write_u64(boot.value());
    hasher.write_u64(publisher.value());
    hasher.write_u64(publisher_boot.value());
    hasher.write_u64(validity.issued_at);
    hasher.write_u64(validity.expires_at);
    hasher.write_u32(bound_flags);
    return hasher.finish();
}

u64 FlowClassification::compute_digest() const {
    Hasher hasher;
    hasher.write_u64(id.value());
    hasher.write_u64(generation.value());
    hasher.write_u64(flow.value());
    hasher.write_u64(flow_generation.value());
    hasher.write_u8(static_cast<u8>(state));
    hasher.write_u8(static_cast<u8>(authority));
    hasher.write_digest(authority_vector.digest());
    hasher.write_u64(static_cast<u64>(reasons));
    hasher.write_u64(decided_at);
    hasher.write_u64(validity.issued_at);
    hasher.write_u64(validity.expires_at);
    hasher.write_digest(measurements.digest());
    hasher.write_digest(impact.digest());
    hasher.write_u32(static_cast<u32>(trace.size()));
    for (const ThresholdTraceEntry& entry : trace) {
        hasher.write_u32(entry.node_index);
        hasher.write_u8(static_cast<u8>(entry.kind));
        hasher.write_u8(static_cast<u8>(entry.result));
        hasher.write_bool(entry.observed_defined);
        hasher.write_u64(entry.observed_value);
        hasher.write_u64(entry.threshold_value);
    }
    hasher.write_bool(trace_truncated);
    hasher.write_u32(enter_streak);
    hasher.write_u32(exit_streak);
    hasher.write_digest(history_digest);
    return hasher.finish();
}

// --- ClassificationHistory --------------------------------------------------

Status ClassificationHistory::record(const ClassificationTransition& transition) {
    if (!entries_.empty()) {
        const ClassificationTransition& last = entries_.back();
        if (transition.at < last.at) {
            return make_status(StatusCode::Stale, "transition tick regressed below the last entry");
        }
        if (transition.from != last.to) {
            return make_status(StatusCode::Conflict,
                               "transition does not continue the recorded classification chain");
        }
    }
    Hasher hasher;
    hasher.write_digest(digest_);
    hasher.write_u64(transition.at);
    hasher.write_u8(static_cast<u8>(transition.from));
    hasher.write_u8(static_cast<u8>(transition.to));
    hasher.write_u8(static_cast<u8>(transition.authority));
    hasher.write_u64(static_cast<u64>(transition.reasons));
    hasher.write_u64(transition.classification.value());
    hasher.write_u64(transition.classification_generation.value());
    digest_ = hasher.finish();

    if (entries_.size() >= max_entries_) {
        entries_.erase(entries_.begin());
        ++folded_count_;
    }
    entries_.push_back(transition);
    return {};
}

std::vector<ClassificationTransition> ClassificationHistory::transitions() const {
    return entries_;
}

void ClassificationHistory::clear() noexcept {
    entries_.clear();
    digest_ = 0;
    folded_count_ = 0;
}

}  // namespace efg
