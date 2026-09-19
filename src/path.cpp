// Elephant Flow Governor - path implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/path.hpp"

#include <algorithm>

namespace efg {

Status PathDescriptor::validate() const {
    if (!path.valid()) {
        return make_status(StatusCode::InvalidArgument, "path identifier is absent");
    }
    if (!generation.valid()) {
        return make_status(StatusCode::InvalidArgument, "path generation is absent");
    }
    if (resources.empty()) {
        return make_status(StatusCode::InvalidArgument, "path must bind at least one resource");
    }
    if (resources.size() > limits::kMaxResourcesPerPath) {
        return make_status(StatusCode::Oversized, "path binds more resources than the ceiling allows");
    }
    std::vector<ResourceId> sorted{resources};
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return make_status(StatusCode::ContradictoryEvidence,
                           "path binds the same resource more than once");
    }
    for (const ResourceId resource : resources) {
        if (!resource.valid()) {
            return make_status(StatusCode::InvalidArgument, "path contains an absent resource");
        }
    }
    if (!validity.well_formed()) {
        return make_status(StatusCode::InvalidArgument, "path validity window is malformed");
    }
    if (!provenance.valid()) {
        return make_status(StatusCode::InvalidArgument, "path provenance is incomplete");
    }
    if (provenance.schema_version != kSchemaVersion) {
        return make_status(StatusCode::VersionMismatch, "path schema version is not supported");
    }
    return {};
}

u64 PathDescriptor::digest() const {
    Hasher hasher;
    hasher.write_u64(path.value());
    hasher.write_u64(generation.value());
    hasher.write_u32(static_cast<u32>(resources.size()));
    for (const ResourceId resource : resources) {
        hasher.write_u64(resource.value());
    }
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

Status PathTable::submit(const PathDescriptor& path) {
    EFG_TRY(path.validate());
    if (path.resources.size() > limits_.max_resources_per_path) {
        return make_status(StatusCode::Oversized, "path exceeds the configured resource ceiling");
    }
    const auto it = entries_.find(path.path);
    if (it == entries_.end()) {
        if (entries_.size() >= limits_.max_paths) {
            return make_status(StatusCode::CapacityExceeded, "path table is full");
        }
        entries_.emplace(path.path, path);
        return {};
    }
    const PathDescriptor& existing = it->second;
    if (path.generation < existing.generation) {
        return make_status(StatusCode::Stale, "path generation is older than the retained one");
    }
    if (path.generation == existing.generation && path.validity.issued_at < existing.validity.issued_at) {
        return make_status(StatusCode::Stale, "path descriptor is older than the retained one");
    }
    if (path.generation == existing.generation && path.digest() == existing.digest()) {
        return {};  // idempotent re-submission
    }
    if (path.generation == existing.generation &&
        path.validity.issued_at == existing.validity.issued_at &&
        path.resources != existing.resources) {
        return make_status(StatusCode::ContradictoryEvidence,
                           "same path generation and issue tick with a different resource set");
    }
    it->second = path;
    return {};
}

Status PathTable::expire(Tick now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.validity.expired_at(now) || !it->second.validity.well_formed()) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    return {};
}

const PathDescriptor* PathTable::find(PathId path) const {
    const auto it = entries_.find(path);
    return it == entries_.end() ? nullptr : &it->second;
}

std::vector<PathId> PathTable::paths() const {
    std::vector<PathId> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.push_back(entry.first);
    }
    return out;
}

}  // namespace efg
