// Elephant Flow Governor - shared test fixtures.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef EFG_TEST_FIXTURES_HPP
#define EFG_TEST_FIXTURES_HPP

#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include "efg/efg.hpp"
#include "test_harness.hpp"

namespace efgtest {

inline constexpr efg::Tick kStart = 1000;

/// A token unique to one process and one call site invocation. Scratch
/// directories are named with it so that two test binaries running at the same
/// time on the same machine cannot share durable state by accident.
[[nodiscard]] inline std::string unique_scratch_suffix() {
    static std::atomic<unsigned> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const unsigned index = counter.fetch_add(1);
    return std::to_string(now) + "-" + std::to_string(index);
}

inline efg::BootIdentity boot(std::uint64_t boot_id = 1, std::uint64_t epoch = 1) {
    efg::BootIdentity identity;
    identity.boot = efg::BootId{boot_id};
    identity.epoch = efg::EpochId{epoch};
    identity.started_at = kStart;
    identity.monotonic_seed = boot_id * 0x9E3779B97F4A7C15ull;
    return identity;
}

inline efg::Provenance provenance(std::uint64_t publisher = 1, std::uint64_t boot_id = 1,
                                  std::uint64_t epoch = 1, std::uint64_t sequence = 1,
                                  efg::Tick at = kStart) {
    efg::Provenance out;
    out.publisher.publisher = efg::PublisherId{publisher};
    out.publisher.boot = efg::BootId{boot_id};
    out.publisher.epoch = efg::EpochId{epoch};
    out.publisher.sequence = sequence;
    out.emitted_at = at;
    out.schema_version = efg::kSchemaVersion;
    return out;
}

/// Policy that qualifies a flow on volume, duration, sustained rate and share.
/// One confirmation window to enter, three to leave.
inline efg::PolicyDocument default_policy(efg::Tick issued = kStart,
                                          std::uint64_t validity = 100000,
                                          efg::u64 rate_threshold = 2000000,
                                          efg::u64 cumulative_threshold = 100000,
                                          efg::u64 duration_threshold = 8,
                                          efg::u32 share_threshold = 300) {
    using namespace efg;
    PolicyDocument policy;
    policy.id = PolicyId{1};
    policy.generation = Generation::initial();
    RuleSet rules;
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::CumulativeBytes, cumulative_threshold,
                                             ServiceClass::Unknown},
                                   {}});
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::DurationTicks, duration_threshold,
                                             ServiceClass::Unknown},
                                   {}});
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::SustainedRate, rate_threshold,
                                             ServiceClass::Unknown},
                                   {}});
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::ShareOfCapacity, share_threshold,
                                             ServiceClass::Unknown},
                                   {}});
    RuleNode root;
    root.kind = RuleKind::All;
    root.children = {0, 1, 2, 3};
    rules.nodes.push_back(root);
    rules.root = 4;
    policy.enter_rule = rules;
    policy.hysteresis.enter_confirm_windows = 1;
    policy.hysteresis.exit_confirm_windows = 3;
    policy.hysteresis.exit_scale_bp = 8000;
    policy.authorization.max_shaping_reduction_bp = 2500;
    policy.authorization.max_intent_duration_ticks = 200;
    policy.authorization.max_targets = 4;
    policy.authorization.min_impact_severity = 1;
    policy.authorization.min_contention_pressure = 1;
    StatusOr<ValidityWindow> window = make_validity(issued, validity);
    if (window.ok()) {
        policy.validity = window.value();
    }
    policy.provenance = provenance(1, 1, 1, 1, issued);
    (void)policy.finalize();
    return policy;
}

inline efg::ResourceCapacity capacity(efg::ResourceId resource, efg::u64 total, efg::u64 reserved,
                                      efg::Tick at = kStart, std::uint64_t validity = 100000,
                                      efg::Generation generation = efg::Generation::initial(),
                                      efg::u64 snapshot = 0) {
    using namespace efg;
    ResourceCapacity out;
    out.resource = resource;
    out.generation = generation;
    out.snapshot = CapacitySnapshotId{snapshot == 0 ? resource.value() : snapshot};
    out.span = TickSpan{at, at + 1};
    out.capacity = total;
    out.reserved = reserved;
    StatusOr<ValidityWindow> window = make_validity(at, validity);
    if (window.ok()) {
        out.validity = window.value();
    }
    out.provenance = provenance(1, 1, 1, resource.value(), at);
    return out;
}

inline efg::PathDescriptor path(efg::PathId id, std::vector<efg::ResourceId> resources,
                                efg::Tick at = kStart, std::uint64_t validity = 100000,
                                efg::Generation generation = efg::Generation::initial()) {
    using namespace efg;
    PathDescriptor out;
    out.path = id;
    out.generation = generation;
    out.resources = std::move(resources);
    StatusOr<ValidityWindow> window = make_validity(at, validity);
    if (window.ok()) {
        out.validity = window.value();
    }
    out.provenance = provenance(1, 1, 1, id.value(), at);
    return out;
}

/// One evidence window. window_bytes is derived from the rate and the span.
inline efg::FlowSample sample(efg::FlowId flow, efg::EvidenceWindowId window, efg::Tick begin,
                              efg::Tick end, efg::u64 cumulative_bytes, efg::u64 window_bytes,
                              efg::PathId path_id = efg::PathId{1},
                              efg::Generation path_generation = efg::Generation::initial(),
                              efg::SequenceNumber sequence = 1,
                              efg::Generation flow_generation = efg::Generation::initial(),
                              efg::u64 validity_span = 100000) {
    using namespace efg;
    FlowSample out;
    out.flow = flow;
    out.flow_generation = flow_generation;
    out.path = path_id;
    out.path_generation = path_generation;
    out.window = window;
    out.window_sequence = sequence;
    out.span = TickSpan{begin, end};
    out.cumulative_bytes = cumulative_bytes;
    out.window_bytes = window_bytes;
    out.service_class = ServiceClass::Standard;
    out.priority = Priority{3};
    out.protection = ProtectionState::Unprotected;
    StatusOr<ValidityWindow> validity = make_validity(begin, validity_span);
    if (validity.ok()) {
        out.validity = validity.value();
    }
    out.provenance = provenance(1, 1, 1, flow.value() * 1000 + sequence, begin);
    out.provenance.digest = out.digest();
    return out;
}

/// Install a policy, one capacity resource and one path bound to it.
inline void install_standard(efg::Governor& governor, efg::Tick at = kStart,
                             efg::u64 total_capacity = 10000000, efg::u64 reserved = 1000000,
                             std::uint64_t policy_validity = 100000) {
    EFG_CHECK_STATUS_OK(governor.submit_policy(default_policy(at, policy_validity)));
    EFG_CHECK_STATUS_OK(governor.submit_capacity(capacity(efg::ResourceId{1}, total_capacity,
                                                          reserved, at, policy_validity)));
    EFG_CHECK_STATUS_OK(
        governor.submit_path(path(efg::PathId{1}, {efg::ResourceId{1}}, at, policy_validity)));
}

/// Feed n contiguous windows of a constant rate to one flow and return the last
/// decision tick. window_ticks is the window length in logical ticks.
inline efg::Tick feed_constant(efg::Governor& governor, efg::FlowId flow, efg::u64 bytes_per_tick,
                               efg::u64 window_ticks, efg::u64 windows, efg::Tick begin = kStart,
                               efg::PathId path_id = efg::PathId{1},
                               efg::Generation flow_generation = efg::Generation::initial()) {
    efg::u64 cumulative = 0;
    efg::Tick tick = begin;
    for (efg::u64 w = 0; w < windows; ++w) {
        const efg::Tick window_begin = begin + w * window_ticks;
        const efg::Tick window_end = window_begin + window_ticks;
        const efg::u64 window_bytes = bytes_per_tick * window_ticks;
        cumulative += window_bytes;
        const efg::FlowSample value =
            sample(flow, efg::EvidenceWindowId{w + 1}, window_begin, window_end, cumulative,
                   window_bytes, path_id, efg::Generation::initial(), w + 1, flow_generation);
        EFG_CHECK_OK(governor.submit_evidence(value));
        tick = window_end;
    }
    return tick;
}

/// Emits one evidence window at a time at a caller chosen rate. The cursor and
/// cumulative volume are carried forward, so the stream is always contiguous.
class WindowFeeder {
public:
    WindowFeeder(efg::Governor& governor, efg::FlowId flow, efg::u64 window_ticks,
                 efg::Tick begin = kStart, efg::PathId path_id = efg::PathId{1},
                 efg::Generation flow_generation = efg::Generation::initial(),
                 efg::Generation path_generation = efg::Generation::initial(),
                 efg::u64 initial_cumulative = 0, efg::u64 initial_sequence = 0)
        : governor_(governor), flow_(flow), window_ticks_(window_ticks), cursor_(begin),
          path_(path_id), flow_generation_(flow_generation), path_generation_(path_generation),
          cumulative_(initial_cumulative), sequence_(initial_sequence) {}

    /// Push one window at the supplied bytes-per-tick rate and return the state
    /// the governor reported for it.
    efg::ElephantState push(efg::u64 bytes_per_tick, efg::u64 validity_span = 100000) {
        const efg::Tick window_begin = cursor_;
        const efg::Tick window_end = window_begin + window_ticks_;
        const efg::u64 window_bytes = bytes_per_tick * window_ticks_;
        cumulative_ += window_bytes;
        ++sequence_;
        const efg::FlowSample value =
            sample(flow_, efg::EvidenceWindowId{sequence_}, window_begin, window_end, cumulative_,
                   window_bytes, path_, path_generation_, sequence_, flow_generation_,
                   validity_span);
        efg::StatusOr<efg::FlowDecision> decision = governor_.submit_evidence(value);
        EFG_CHECK_OK(decision);
        cursor_ = window_end;
        return decision.value().classification.state;
    }

    /// Push one window with explicit control over protection metadata.
    efg::ElephantState push_protected(efg::u64 bytes_per_tick, efg::ProtectionState protection,
                                      efg::ServiceClass service_class, bool with_reservation) {
        const efg::Tick window_begin = cursor_;
        const efg::Tick window_end = window_begin + window_ticks_;
        const efg::u64 window_bytes = bytes_per_tick * window_ticks_;
        cumulative_ += window_bytes;
        ++sequence_;
        efg::FlowSample value =
            sample(flow_, efg::EvidenceWindowId{sequence_}, window_begin, window_end, cumulative_,
                   window_bytes, path_, path_generation_, sequence_, flow_generation_);
        value.protection = protection;
        value.service_class = service_class;
        if (with_reservation) {
            value.reservation = efg::ReservationId{77};
        }
        value.provenance.digest = value.digest();
        efg::StatusOr<efg::FlowDecision> decision = governor_.submit_evidence(value);
        EFG_CHECK_OK(decision);
        cursor_ = window_end;
        return decision.value().classification.state;
    }

    [[nodiscard]] efg::Tick cursor() const { return cursor_; }
    [[nodiscard]] efg::u64 sequence() const { return sequence_; }
    [[nodiscard]] efg::u64 cumulative() const { return cumulative_; }

private:
    efg::Governor& governor_;
    efg::FlowId flow_;
    efg::u64 window_ticks_;
    efg::Tick cursor_;
    efg::PathId path_;
    efg::Generation flow_generation_;
    efg::Generation path_generation_;
    efg::u64 cumulative_{0};
    efg::u64 sequence_{0};
};

/// Push enough windows at the supplied rate for the standard policy to qualify
/// the flow, and return the state the governor reported for the final window.
inline efg::ElephantState qualify(WindowFeeder& feeder, efg::u64 bytes_per_tick = 9000,
                                  int windows = 4) {
    efg::ElephantState state = efg::ElephantState::Unknown;
    for (int i = 0; i < windows; ++i) {
        state = feeder.push(bytes_per_tick);
    }
    return state;
}

/// Qualify a flow that carries protection metadata on every window.
inline efg::ElephantState qualify_protected(WindowFeeder& feeder, efg::ProtectionState protection,
                                           efg::ServiceClass service_class, bool with_reservation,
                                           efg::u64 bytes_per_tick = 3000, int windows = 4) {
    efg::ElephantState state = efg::ElephantState::Unknown;
    for (int i = 0; i < windows; ++i) {
        state = feeder.push_protected(bytes_per_tick, protection, service_class, with_reservation);
    }
    return state;
}

/// Policy with caller chosen hysteresis, used by the anti-flapping tests.
inline efg::PolicyDocument hysteresis_policy(efg::u32 enter_confirm, efg::u32 exit_confirm,
                                             efg::u64 rate_threshold = 2000000,
                                             efg::u64 cumulative_threshold = 100000,
                                             efg::Tick issued = kStart,
                                             std::uint64_t validity = 100000) {
    efg::PolicyDocument policy = default_policy(issued, validity, rate_threshold,
                                                cumulative_threshold, 8, 300);
    policy.hysteresis.enter_confirm_windows = enter_confirm;
    policy.hysteresis.exit_confirm_windows = exit_confirm;
    policy.hysteresis.exit_scale_bp = 8000;
    (void)policy.finalize();
    return policy;
}

}  // namespace efgtest

#endif  // EFG_TEST_FIXTURES_HPP
