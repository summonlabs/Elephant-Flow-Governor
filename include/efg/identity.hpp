// Elephant Flow Governor - strong identities, generations and provenance.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every authoritative record in this runtime is bound to the exact identities
// and generations that justified it. Identifiers are distinct types, so a path
// identifier can never be passed where a resource identifier is expected, and a
// generation can never be silently widened into an identifier.
//
// Identifier value 0 is reserved and means "absent". Generations start at 1 and
// strictly increase. Advancing a generation is checked; an exhausted generation
// is a refusal, never a wrap.

#ifndef EFG_IDENTITY_HPP
#define EFG_IDENTITY_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "efg/checked.hpp"
#include "efg/hash.hpp"
#include "efg/limits.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"
#include "efg/version.hpp"

namespace efg {

/// Strongly typed 64 bit identifier. The tag type is never instantiated; it
/// exists only to make the types incompatible.
template <typename Tag>
class StrongId {
public:
    using rep = std::uint64_t;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(rep value) noexcept : value_(value) {}

    [[nodiscard]] constexpr rep value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
    [[nodiscard]] constexpr bool absent() const noexcept { return value_ == 0; }

    friend constexpr bool operator==(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }
    friend constexpr bool operator!=(StrongId lhs, StrongId rhs) noexcept { return !(lhs == rhs); }
    friend constexpr bool operator<(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ < rhs.value_;
    }
    friend constexpr bool operator>(StrongId lhs, StrongId rhs) noexcept { return rhs < lhs; }
    friend constexpr bool operator<=(StrongId lhs, StrongId rhs) noexcept { return !(rhs < lhs); }
    friend constexpr bool operator>=(StrongId lhs, StrongId rhs) noexcept { return !(lhs < rhs); }

    constexpr void write_to(Hasher& hasher) const noexcept { hasher.write_u64(value_); }

private:
    rep value_{0};
};

struct FlowIdTag {};
struct PathIdTag {};
struct ResourceIdTag {};
struct EvidenceWindowIdTag {};
struct CapacitySnapshotIdTag {};
struct PolicyIdTag {};
struct ClassificationIdTag {};
struct IntentIdTag {};
struct AttemptIdTag {};
struct EpochIdTag {};
struct BootIdTag {};
struct PublisherIdTag {};
struct WorkerIdTag {};
struct ReservationIdTag {};
struct ServiceClassNameTag {};
struct TenantIdTag {};

using FlowId = StrongId<FlowIdTag>;
using PathId = StrongId<PathIdTag>;
using ResourceId = StrongId<ResourceIdTag>;
using EvidenceWindowId = StrongId<EvidenceWindowIdTag>;
using CapacitySnapshotId = StrongId<CapacitySnapshotIdTag>;
using PolicyId = StrongId<PolicyIdTag>;
using ClassificationId = StrongId<ClassificationIdTag>;
using IntentId = StrongId<IntentIdTag>;
using AttemptId = StrongId<AttemptIdTag>;
using EpochId = StrongId<EpochIdTag>;
using BootId = StrongId<BootIdTag>;
using PublisherId = StrongId<PublisherIdTag>;
using WorkerId = StrongId<WorkerIdTag>;
using ReservationId = StrongId<ReservationIdTag>;
using ServiceClassName = StrongId<ServiceClassNameTag>;
using TenantId = StrongId<TenantIdTag>;

/// A monotonically increasing generation. Generation 0 is reserved for "none".
class Generation {
public:
    constexpr Generation() noexcept = default;
    constexpr explicit Generation(u64 value) noexcept : value_(value) {}

    [[nodiscard]] static constexpr Generation initial() noexcept { return Generation{1}; }
    [[nodiscard]] constexpr u64 value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    /// Advance to the next generation. Refuses on exhaustion rather than wrapping.
    [[nodiscard]] StatusOr<Generation> next() const noexcept {
        if (value_ == UINT64_MAX) {
            return make_status(StatusCode::Overflow, "generation counter exhausted");
        }
        return Generation{value_ + 1};
    }

    friend constexpr bool operator==(Generation lhs, Generation rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }
    friend constexpr bool operator!=(Generation lhs, Generation rhs) noexcept {
        return !(lhs == rhs);
    }
    friend constexpr bool operator<(Generation lhs, Generation rhs) noexcept {
        return lhs.value_ < rhs.value_;
    }
    friend constexpr bool operator>(Generation lhs, Generation rhs) noexcept { return rhs < lhs; }
    friend constexpr bool operator<=(Generation lhs, Generation rhs) noexcept { return !(rhs < lhs); }
    friend constexpr bool operator>=(Generation lhs, Generation rhs) noexcept { return !(lhs < rhs); }

    constexpr void write_to(Hasher& hasher) const noexcept { hasher.write_u64(value_); }

private:
    u64 value_{0};
};

/// A monotonically increasing sequence number within a publisher incarnation.
using SequenceNumber = std::uint64_t;

/// Fail-closed comparison result for identity/generation pairs.
enum class BindingVerdict : std::uint8_t {
    Match = 0,
    FlowGenerationChanged = 1,
    PathGenerationChanged = 2,
    CapacityGenerationChanged = 3,
    PolicyGenerationChanged = 4,
    EvidenceWindowSuperseded = 5,
    EpochChanged = 6,
    BootChanged = 7,
    PublisherChanged = 8,
    PublisherIncarnationChanged = 9,
    Absent = 10,
};

[[nodiscard]] std::string_view to_string(BindingVerdict verdict) noexcept;

/// Identity of a publisher incarnation: a publisher is only trusted within the
/// exact process incarnation and cluster epoch that emitted the evidence.
struct PublisherRef {
    PublisherId publisher{};
    BootId boot{};
    EpochId epoch{};
    SequenceNumber sequence{0};

    [[nodiscard]] constexpr bool valid() const noexcept { return publisher.valid() && boot.valid(); }

    friend constexpr bool operator==(const PublisherRef&, const PublisherRef&) noexcept = default;
};

/// Full provenance attached to every externally supplied record.
struct Provenance {
    PublisherRef publisher{};
    Tick emitted_at{0};
    u32 schema_version{kSchemaVersion};
    u64 digest{0};  // integrity digest of the payload this provenance describes

    [[nodiscard]] constexpr bool valid() const noexcept {
        return publisher.valid() && schema_version != 0;
    }

    friend constexpr bool operator==(const Provenance&, const Provenance&) noexcept = default;
};

/// Identity of a single boot of a process. A boot identifier is minted fresh on
/// every start; durable state must never resurrect authority from a prior boot.
struct BootIdentity {
    BootId boot{};
    EpochId epoch{};
    Tick started_at{0};
    u64 monotonic_seed{0};

    [[nodiscard]] constexpr bool valid() const noexcept { return boot.valid(); }
    friend constexpr bool operator==(const BootIdentity&, const BootIdentity&) noexcept = default;
};

/// Monotonic counter that refuses to wrap, used for epochs and incarnations.
class EpochCounter {
public:
    EpochCounter() = default;
    explicit EpochCounter(EpochId initial) noexcept : current_(initial) {}

    [[nodiscard]] EpochId current() const noexcept { return current_; }

    [[nodiscard]] StatusOr<EpochId> advance() noexcept {
        if (current_.value() == UINT64_MAX) {
            return make_status(StatusCode::Overflow, "epoch counter exhausted");
        }
        current_ = EpochId{current_.value() + 1};
        return current_;
    }

    void force(EpochId value) noexcept { current_ = value; }

private:
    EpochId current_{};
};

/// Monotonic advance of a generation value with a labelled refusal.
[[nodiscard]] inline StatusOr<Generation> advance_generation(Generation current,
                                                            std::string_view what) noexcept {
    StatusOr<Generation> next = current.next();
    if (!next.ok()) {
        return make_status(StatusCode::Overflow, what);
    }
    return next;
}

}  // namespace efg

namespace std {

template <typename Tag>
struct hash<efg::StrongId<Tag>> {
    [[nodiscard]] size_t operator()(efg::StrongId<Tag> id) const noexcept {
        efg::u64 z = id.value() + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<size_t>(z ^ (z >> 31));
    }
};

template <>
struct hash<efg::Generation> {
    [[nodiscard]] size_t operator()(efg::Generation generation) const noexcept {
        efg::u64 z = generation.value() + 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return static_cast<size_t>(z ^ (z >> 31));
    }
};

}  // namespace std

#endif  // EFG_IDENTITY_HPP
