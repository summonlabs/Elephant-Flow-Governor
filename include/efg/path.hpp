// Elephant Flow Governor - path evidence.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A path binds an ordered list of capacity resources. It carries a generation so
// that a re-route - even onto an identical resource set - is visible as a change
// and invalidates classifications that were justified against the old binding.

#ifndef EFG_PATH_HPP
#define EFG_PATH_HPP

#include <cstdint>
#include <map>
#include <vector>

#include "efg/checked.hpp"
#include "efg/hash.hpp"
#include "efg/identity.hpp"
#include "efg/limits.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"

namespace efg {

struct PathDescriptor {
    PathId path{};
    Generation generation{};
    std::vector<ResourceId> resources{};
    ValidityWindow validity{};
    Provenance provenance{};

    [[nodiscard]] Status validate() const;
    [[nodiscard]] u64 digest() const;
};

void encode(ByteWriter& writer, const PathDescriptor& path);

/// Bounded table of path generations.
class PathTable {
public:
    struct Limits {
        std::size_t max_paths{limits::kMaxPaths};
        std::size_t max_resources_per_path{limits::kMaxResourcesPerPath};
    };

    PathTable() = default;
    explicit PathTable(Limits limits) : limits_(limits) {}

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    Status submit(const PathDescriptor& path);
    Status expire(Tick now);

    [[nodiscard]] const PathDescriptor* find(PathId path) const;
    [[nodiscard]] std::vector<PathId> paths() const;

    void clear() noexcept { entries_.clear(); }

private:
    Limits limits_{};
    std::map<PathId, PathDescriptor> entries_;
};

}  // namespace efg

#endif  // EFG_PATH_HPP
