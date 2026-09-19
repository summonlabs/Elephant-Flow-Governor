// Elephant Flow Governor - capacity implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/capacity.hpp"

namespace efg {

Status ResourceCapacity::validate() const {
    if (!resource.valid()) {
        return make_status(StatusCode::InvalidArgument, "resource identifier is absent");
    }
    if (!generation.valid()) {
        return make_status(StatusCode::InvalidArgument, "capacity generation is absent");
    }
    if (!snapshot.valid()) {
        return make_status(StatusCode::InvalidArgument, "capacity snapshot identifier is absent");
    }
    if (span.end <= span.begin) {
        return make_status(StatusCode::InvalidArgument, "capacity span is empty or inverted");
    }
    if (capacity == 0) {
        return make_status(StatusCode::InvalidArgument, "capacity must be positive");
    }
    if (reserved > capacity) {
        return make_status(StatusCode::ContradictoryEvidence,
                           "reserved capacity exceeds total capacity");
    }
    if (!validity.well_formed()) {
        return make_status(StatusCode::InvalidArgument, "capacity validity window is malformed");
    }
    if (validity.expires_at <= span.end) {
        return make_status(StatusCode::Expired,
                           "capacity validity expires before the measured span ends");
    }
    if (!provenance.valid()) {
        return make_status(StatusCode::InvalidArgument, "capacity provenance is incomplete");
    }
    if (provenance.schema_version != kSchemaVersion) {
        return make_status(StatusCode::VersionMismatch, "capacity schema version is not supported");
    }
    return {};
}

u64 ResourceCapacity::digest() const {
    Hasher hasher;
    hasher.write_u64(resource.value());
    hasher.write_u64(generation.value());
    hasher.write_u64(snapshot.value());
    hasher.write_u64(span.begin);
    hasher.write_u64(span.end);
    hasher.write_u64(capacity);
    hasher.write_u64(reserved);
    hasher.write_u64(validity.issued_at);
    hasher.write_u64(validity.expires_at);
    hasher.write_u64(provenance.publisher.publisher.value());
    hasher.write_u64(provenance.publisher.boot.value());
    hasher.write_u64(provenance.publisher.epoch.value());
    hasher.write_u64(provenance.publisher.sequence);
    hasher.write_u64(provenance.emitted_at);
    hasher.write_u32(provenance.schema_version);
    return hasher.finish();
}

Status CapacityTable::submit(const ResourceCapacity& snapshot) {
    EFG_TRY(snapshot.validate());
    const auto it = entries_.find(snapshot.resource);
    if (it == entries_.end()) {
        if (entries_.size() >= limits_.max_resources) {
            return make_status(StatusCode::CapacityExceeded, "capacity table is full");
        }
        entries_.emplace(snapshot.resource, snapshot);
        return {};
    }
    const ResourceCapacity& existing = it->second;
    if (snapshot.generation < existing.generation) {
        return make_status(StatusCode::Stale,
                           "capacity snapshot generation is older than the retained one");
    }
    if (snapshot.generation == existing.generation &&
        snapshot.validity.issued_at < existing.validity.issued_at) {
        return make_status(StatusCode::Stale,
                           "capacity snapshot is older than the retained one for this generation");
    }
    if (snapshot.generation == existing.generation && snapshot.snapshot == existing.snapshot) {
        if (snapshot.digest() == existing.digest()) {
            return {};  // idempotent re-submission
        }
        if (snapshot.validity.issued_at == existing.validity.issued_at) {
            return make_status(StatusCode::ContradictoryEvidence,
                               "same capacity snapshot identity with different content");
        }
    }
    it->second = snapshot;
    return {};
}

const ResourceCapacity* CapacityTable::find(ResourceId resource) const {
    const auto it = entries_.find(resource);
    return it == entries_.end() ? nullptr : &it->second;
}

Status CapacityTable::expire(Tick now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.validity.expired_at(now) || !it->second.validity.well_formed()) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    return {};
}

std::vector<ResourceId> CapacityTable::resources() const {
    std::vector<ResourceId> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.push_back(entry.first);
    }
    return out;
}

}  // namespace efg
