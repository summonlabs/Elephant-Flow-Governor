// Elephant Flow Governor - flow evidence.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// A flow is identified by (FlowId, Generation). The generation is what makes
// classification authority revocable: a rollover or an identity reuse advances
// the generation and every classification bound to the old generation becomes
// void, whether or not anybody remembered to revoke it explicitly.
//
// Evidence arrives as non-overlapping, strictly ordered windows. Each window
// carries both the cumulative volume since the flow generation started and the
// volume observed inside that window, so a replay, a duplicate or a contradiction
// is detectable rather than silently absorbed.

#ifndef EFG_FLOW_HPP
#define EFG_FLOW_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "efg/checked.hpp"
#include "efg/hash.hpp"
#include "efg/identity.hpp"
#include "efg/limits.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"
#include "efg/tri.hpp"

namespace efg {

/// Service class as reported by the authoritative source. Unknown is a real,
/// first-class value: it is never coerced into BestEffort.
enum class ServiceClass : std::uint8_t {
    Unknown = 0,
    BestEffort = 1,
    Standard = 2,
    Priority = 3,
    Reserved = 4,
    Control = 5,
};

[[nodiscard]] std::string_view to_string(ServiceClass value) noexcept;
[[nodiscard]] bool parse_service_class(std::string_view text, ServiceClass& out) noexcept;

/// Protection state as reported by the authoritative source.
enum class ProtectionState : std::uint8_t {
    Unknown = 0,
    Unprotected = 1,
    Protected = 2,
    NonPreemptible = 3,
};

[[nodiscard]] std::string_view to_string(ProtectionState value) noexcept;
[[nodiscard]] bool parse_protection_state(std::string_view text, ProtectionState& out) noexcept;

/// Scheduling priority with an explicit unknown state. Levels run 0 (lowest)
/// through 7 (highest).
class Priority {
public:
    static constexpr u8 kMaxLevel = 7;

    constexpr Priority() noexcept = default;
    constexpr explicit Priority(u8 level) noexcept : level_(level), known_(true) {}

    [[nodiscard]] static constexpr Priority unknown() noexcept { return Priority{}; }

    [[nodiscard]] constexpr bool known() const noexcept { return known_; }
    [[nodiscard]] constexpr u8 level() const noexcept { return known_ ? level_ : 0; }

    friend constexpr bool operator==(Priority lhs, Priority rhs) noexcept {
        return lhs.known_ == rhs.known_ && lhs.level_ == rhs.level_;
    }

private:
    u8 level_{0};
    bool known_{false};
};

/// One authoritative observation window for one flow generation.
struct FlowSample {
    FlowId flow{};
    Generation flow_generation{};
    PathId path{};
    Generation path_generation{};
    EvidenceWindowId window{};
    SequenceNumber window_sequence{0};

    /// Half open observation interval. Windows must not overlap.
    TickSpan span{};

    /// Cumulative bytes observed since the flow generation started. Monotone
    /// non-decreasing inside a generation; a decrease means the identity was
    /// reused without advancing the generation and is refused.
    u64 cumulative_bytes{0};

    /// Bytes observed strictly inside this window.
    u64 window_bytes{0};

    ServiceClass service_class{ServiceClass::Unknown};
    Priority priority{};
    ProtectionState protection{ProtectionState::Unknown};
    ReservationId reservation{};
    TenantId tenant{};

    /// How long this observation stays authoritative.
    ValidityWindow validity{};

    Provenance provenance{};

    [[nodiscard]] Status validate() const;
    [[nodiscard]] u64 digest() const;
};

/// Canonical, domain separated encoding of a sample. The digest published by
/// FlowSample::digest() is the digest of exactly these bytes.
void encode(ByteWriter& writer, const FlowSample& sample);

/// Derived measurements of one flow generation. This is the complete input to
/// policy evaluation; policy evaluation is a pure function of it, which is what
/// makes classification reproducible.
struct FlowMeasurements {
    bool evidence_present{false};
    bool evidence_contiguous{false};
    bool gap_detected{false};
    bool capacity_known{false};
    bool path_known{false};

    TickSpan observed{};
    u64 duration_ticks{0};
    u64 cumulative_bytes{0};
    u64 retained_window_bytes{0};

    /// Bytes per one thousand logical ticks.
    KiloTickRate sustained_rate{0};
    KiloTickRate peak_window_rate{0};

    u64 sample_count{0};
    u64 window_count{0};
    u64 dropped_windows{0};

    ServiceClass service_class{ServiceClass::Unknown};
    Priority priority{};
    ProtectionState protection{ProtectionState::Unknown};
    ReservationId reservation{};
    bool has_reservation{false};
    TenantId tenant{};

    /// True when the flow reported a clean completion.
    bool completed{false};

    /// Binding resource of the flow's path: the resource with the least
    /// unreserved capacity. Ties are broken by ascending resource identifier so
    /// the choice is deterministic.
    ResourceId binding_resource{};
    KiloTickRate resource_capacity{0};
    KiloTickRate resource_reserved{0};
    KiloTickRate resource_available{0};

    BasisPoints share_of_capacity{0};
    BasisPoints share_of_available{0};

    u32 competing_flows{0};
    u32 competing_elephants{0};

    /// Retained window volume above the reserved guarantee of the binding resource.
    u64 excess_over_guarantee{0};

    /// True when every field the policy needs is present and not Unknown.
    bool fields_complete{false};

    [[nodiscard]] u64 digest() const;
};

/// Outcome of appending a sample to a ledger.
enum class AppendOutcome : std::uint8_t {
    Accepted = 0,
    DuplicateIgnored = 1,
    Completed = 2,
};

/// Bounded evidence accumulator for exactly one (FlowId, Generation) pair.
///
/// The ledger keeps at most limits::kMaxSamplesPerFlow windows. Evicting an old
/// window never widens the retained rate window silently: the ledger records how
/// many windows it dropped so explanations can say so.
class FlowLedger {
public:
    struct Limits {
        Tick max_gap_ticks{0};           // 0 disables gap detection
        std::size_t max_samples{limits::kMaxSamplesPerFlow};
        u64 max_bytes_per_window{0};     // 0 disables the per-window ceiling
        u64 max_cumulative_bytes{0};     // 0 disables the cumulative ceiling
    };

    FlowLedger() = default;
    FlowLedger(FlowId flow, Generation generation, Limits limits);

    [[nodiscard]] FlowId flow() const noexcept { return flow_; }
    [[nodiscard]] Generation generation() const noexcept { return generation_; }
    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }

    [[nodiscard]] StatusOr<AppendOutcome> append(const FlowSample& sample);

    /// Mark the flow as cleanly completed at the supplied tick. Completion is
    /// recorded evidence, not a silent state transition: it fences governance
    /// intent but does not by itself erase classification history.
    Status mark_completed(Tick completed_at);

    [[nodiscard]] bool completed() const noexcept { return completed_; }
    [[nodiscard]] Tick completed_at() const noexcept { return completed_at_; }

    [[nodiscard]] std::size_t size() const noexcept { return samples_.size(); }
    [[nodiscard]] const std::vector<FlowSample>& samples() const noexcept { return samples_; }
    [[nodiscard]] const FlowSample* latest() const noexcept {
        return samples_.empty() ? nullptr : &samples_.back();
    }
    [[nodiscard]] const ValidityWindow& latest_validity() const noexcept { return latest_validity_; }
    [[nodiscard]] bool gap_detected() const noexcept { return gap_detected_; }

    /// True when the flow reported on a different path generation during this
    /// generation's lifetime. The retained measurement window restarts at that
    /// point so a rate is never computed across two different path bindings.
    [[nodiscard]] bool path_changed() const noexcept { return path_changed_; }

    /// Derive measurements using only the evidence currently retained.
    [[nodiscard]] FlowMeasurements measurements() const;

    void clear() noexcept;

private:
    [[nodiscard]] Status check_order(const FlowSample& sample) const;

    FlowId flow_{};
    Generation generation_{};
    Limits limits_{};
    std::vector<FlowSample> samples_;
    ValidityWindow latest_validity_{};
    SequenceNumber last_sequence_{0};
    EvidenceWindowId last_window_{};
    Tick last_span_end_{0};
    u64 last_cumulative_{0};
    PathId last_path_{};
    Generation last_path_generation_{};
    u64 generation_cumulative_{0};
    bool has_previous_{false};
    bool path_changed_{false};
    bool gap_detected_{false};
    bool completed_{false};
    Tick completed_at_{0};
    u64 dropped_windows_{0};
};

/// Free function form of the derivation so it can be unit tested and reused for
/// replayed history without constructing a ledger.
[[nodiscard]] StatusOr<FlowMeasurements> derive_measurements(std::span<const FlowSample> samples,
                                                            const FlowLedger::Limits& limits,
                                                            bool completed,
                                                            bool gap_detected,
                                                            u64 dropped_windows);

}  // namespace efg

#endif  // EFG_FLOW_HPP
