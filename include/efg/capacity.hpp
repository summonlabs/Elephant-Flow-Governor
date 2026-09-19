// Elephant Flow Governor - capacity evidence.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A capacity snapshot is authoritative only inside its validity window and only
// for the exact resource generation it names. Reserved capacity is a protected
// obligation: the governor may never plan an action that consumes it, and a
// capacity snapshot that claims reserved capacity above total capacity is
// structurally invalid and refused outright.

#ifndef EFG_CAPACITY_HPP
#define EFG_CAPACITY_HPP

#include <cstdint>
#include <map>
#include <vector>

#include "efg/checked.hpp"
#include "efg/hash.hpp"
#include "efg/identity.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"
#include "efg/tri.hpp"

namespace efg {

/// One authoritative statement of the capacity of one resource.
struct ResourceCapacity {
    ResourceId resource{};
    Generation generation{};
    CapacitySnapshotId snapshot{};
    TickSpan span{};

    /// Total capacity in bytes per one thousand logical ticks.
    KiloTickRate capacity{0};

    /// Capacity contractually reserved for protected or guaranteed traffic.
    /// Always less than or equal to capacity.
    KiloTickRate reserved{0};

    ValidityWindow validity{};
    Provenance provenance{};

    [[nodiscard]] Status validate() const;

    /// Unreserved capacity available to discretionary traffic.
    [[nodiscard]] StatusOr<KiloTickRate> available() const {
        u64 out = 0;
        if (sub_overflow(capacity, reserved, out)) {
            return make_status(StatusCode::Underflow,
                               "reserved capacity exceeds total capacity");
        }
        return out;
    }

    [[nodiscard]] u64 digest() const;
};

void encode(ByteWriter& writer, const ResourceCapacity& capacity);

/// Bounded table of the most recent capacity snapshot per resource generation.
class CapacityTable {
public:
    struct Limits {
        std::size_t max_resources{limits::kMaxResources};
    };

    CapacityTable() = default;
    explicit CapacityTable(Limits limits) : limits_(limits) {}

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    /// Insert or replace a snapshot. Replacing an older generation is allowed;
    /// inserting an *older* generation than the one on record is refused so that
    /// a delayed duplicate cannot roll capacity backwards.
    Status submit(const ResourceCapacity& snapshot);

    [[nodiscard]] const ResourceCapacity* find(ResourceId resource) const;

    /// Not const-correct to erase lazily, so expiry is an explicit sweep.
    Status expire(Tick now);

    [[nodiscard]] std::vector<ResourceId> resources() const;

    void clear() noexcept { entries_.clear(); }

private:
    Limits limits_{};
    std::map<ResourceId, ResourceCapacity> entries_;
};

}  // namespace efg

#endif  // EFG_CAPACITY_HPP
