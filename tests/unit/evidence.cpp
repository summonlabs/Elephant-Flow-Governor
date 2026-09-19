// Elephant Flow Governor - evidence, capacity and path tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace efg;
using namespace efgtest;

namespace {

FlowLedger::Limits ledger_limits(u64 max_gap = 0, std::size_t samples = 8) {
    FlowLedger::Limits limits;
    limits.max_gap_ticks = max_gap;
    limits.max_samples = samples;
    return limits;
}

}  // namespace

EFG_TEST(evidence, ledger_accepts_ordered_contiguous_windows) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits()};
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                      Generation::initial(), 1)));
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{2}, 110, 120, 2500, 1500, PathId{1},
                                      Generation::initial(), 2)));
    const FlowMeasurements measurements = ledger.measurements();
    EFG_CHECK(measurements.evidence_present);
    EFG_CHECK(measurements.evidence_contiguous);
    EFG_CHECK_EQ(measurements.cumulative_bytes, 2500u);
    EFG_CHECK_EQ(measurements.duration_ticks, 20u);
    EFG_CHECK_EQ(measurements.retained_window_bytes, 2500u);
    EFG_CHECK_EQ(measurements.sustained_rate, 125000u);  // 2500 bytes over 20 ticks, per 1000 ticks
}

EFG_TEST(evidence, a_single_window_is_not_a_sustained_measurement) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits()};
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                      Generation::initial(), 1)));
    const FlowMeasurements measurements = ledger.measurements();
    EFG_CHECK(measurements.evidence_present);
    EFG_CHECK(!measurements.evidence_contiguous);
    const Threshold rate_threshold{ThresholdKind::SustainedRate, 1, ServiceClass::Unknown};
    EFG_CHECK_EQ(rate_threshold.evaluate(measurements), Tri::Unknown);
}

EFG_TEST(evidence, replaying_an_identical_window_is_idempotent) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits()};
    const FlowSample value = sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                    Generation::initial(), 1);
    EFG_CHECK_OK(ledger.append(value));
    const StatusOr<AppendOutcome> second = ledger.append(value);
    EFG_CHECK_OK(second);
    EFG_CHECK_EQ(second.value(), AppendOutcome::DuplicateIgnored);
    EFG_CHECK_EQ(ledger.size(), 1u);
}

EFG_TEST(evidence, same_window_with_different_content_is_contradictory) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits()};
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                      Generation::initial(), 1)));
    FlowSample conflicting = sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                    Generation::initial(), 1);
    conflicting.window_bytes = 999;
    conflicting.provenance.digest = conflicting.digest();
    EFG_CHECK_EQ(ledger.append(conflicting).code(), StatusCode::ContradictoryEvidence);
}

EFG_TEST(evidence, sequence_regression_is_refused) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits()};
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                      Generation::initial(), 5)));
    EFG_CHECK_EQ(ledger.append(sample(FlowId{1}, EvidenceWindowId{2}, 110, 120, 2000, 1000, PathId{1},
                                      Generation::initial(), 4))
                     .code(),
                 StatusCode::ContradictoryEvidence);
}

EFG_TEST(evidence, cumulative_regression_inside_a_generation_is_refused) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits()};
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                      Generation::initial(), 1)));
    // A non contiguous window that reports less cumulative volume than before
    // means the identity was reused without advancing the generation.
    EFG_CHECK_EQ(ledger.append(sample(FlowId{1}, EvidenceWindowId{2}, 200, 210, 500, 500, PathId{1},
                                      Generation::initial(), 2))
                     .code(),
                 StatusCode::ContradictoryEvidence);
}

EFG_TEST(evidence, overlapping_windows_are_refused) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits()};
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                      Generation::initial(), 1)));
    EFG_CHECK_EQ(ledger.append(sample(FlowId{1}, EvidenceWindowId{2}, 105, 115, 2000, 1000, PathId{1},
                                      Generation::initial(), 2))
                     .code(),
                 StatusCode::ContradictoryEvidence);
}

EFG_TEST(evidence, a_contiguous_window_that_disagrees_with_the_delta_is_refused) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits()};
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                      Generation::initial(), 1)));
    EFG_CHECK_EQ(ledger.append(sample(FlowId{1}, EvidenceWindowId{2}, 110, 120, 3000, 1000, PathId{1},
                                      Generation::initial(), 2))
                     .code(),
                 StatusCode::ContradictoryEvidence);
}

EFG_TEST(evidence, a_telemetry_gap_is_detected_and_breaks_contiguity) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits(10)};
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                      Generation::initial(), 1)));
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{2}, 200, 210, 2000, 1000, PathId{1},
                                      Generation::initial(), 2)));
    EFG_CHECK(ledger.gap_detected());
    EFG_CHECK(!ledger.measurements().evidence_contiguous);
    EFG_CHECK(ledger.measurements().gap_detected);
}

EFG_TEST(evidence, a_path_generation_change_restarts_the_measurement_window) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits()};
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                      Generation::initial(), 1)));
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{2}, 110, 120, 2000, 1000, PathId{1},
                                      Generation::initial(), 2)));
    EFG_CHECK_EQ(ledger.size(), 2u);
    // Same path identifier, new path generation: a re-route.
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{3}, 120, 130, 3000, 1000, PathId{1},
                                      Generation{2}, 3)));
    EFG_CHECK(ledger.path_changed());
    EFG_CHECK_EQ(ledger.size(), 1u);
    EFG_CHECK(!ledger.measurements().evidence_contiguous);
    EFG_CHECK_EQ(ledger.measurements().cumulative_bytes, 3000u);
    EFG_CHECK(ledger.measurements().dropped_windows > 0);
}

EFG_TEST(evidence, retention_is_bounded_and_evictions_are_counted) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits(0, 3)};
    u64 cumulative = 0;
    for (u64 w = 0; w < 6; ++w) {
        const Tick begin = 100 + w * 10;
        cumulative += 1000;
        EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{w + 1}, begin, begin + 10,
                                          cumulative, 1000, PathId{1}, Generation::initial(), w + 1)));
    }
    EFG_CHECK_EQ(ledger.size(), 3u);
    EFG_CHECK_EQ(ledger.measurements().dropped_windows, 3u);
    EFG_CHECK(ledger.measurements().evidence_contiguous);
    EFG_CHECK_EQ(ledger.measurements().duration_ticks, 30u);
}

EFG_TEST(evidence, malformed_samples_are_rejected_field_by_field) {
    FlowSample value = sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000);
    EFG_CHECK_STATUS_OK(value.validate());

    FlowSample zero_flow = value;
    zero_flow.flow = FlowId{};
    zero_flow.provenance.digest = zero_flow.digest();
    EFG_CHECK_EQ(zero_flow.validate().code(), StatusCode::InvalidArgument);

    FlowSample zero_generation = value;
    zero_generation.flow_generation = Generation{};
    zero_generation.provenance.digest = zero_generation.digest();
    EFG_CHECK_EQ(zero_generation.validate().code(), StatusCode::InvalidArgument);

    FlowSample inverted = value;
    inverted.span = TickSpan{110, 100};
    inverted.provenance.digest = inverted.digest();
    EFG_CHECK_EQ(inverted.validate().code(), StatusCode::InvalidArgument);

    FlowSample impossible = value;
    impossible.window_bytes = impossible.cumulative_bytes + 1;
    impossible.provenance.digest = impossible.digest();
    EFG_CHECK_EQ(impossible.validate().code(), StatusCode::ContradictoryEvidence);

    FlowSample undated = value;
    undated.validity = ValidityWindow{};
    undated.provenance.digest = undated.digest();
    EFG_CHECK_EQ(undated.validate().code(), StatusCode::InvalidArgument);

    FlowSample wrong_schema = value;
    wrong_schema.provenance.schema_version = kSchemaVersion + 1;
    wrong_schema.provenance.digest = wrong_schema.digest();
    EFG_CHECK_EQ(wrong_schema.validate().code(), StatusCode::VersionMismatch);

    FlowSample tampered = value;
    tampered.window_bytes = 1;
    EFG_CHECK_EQ(tampered.validate().code(), StatusCode::IntegrityFailure);

    FlowSample bad_priority = value;
    bad_priority.priority = Priority{9};
    bad_priority.provenance.digest = bad_priority.digest();
    EFG_CHECK_EQ(bad_priority.validate().code(), StatusCode::OutOfRange);
}

EFG_TEST(evidence, ledger_refuses_bound_volume_ceilings) {
    FlowLedger::Limits limits = ledger_limits();
    limits.max_bytes_per_window = 500;
    limits.max_cumulative_bytes = 1000;
    FlowLedger ledger{FlowId{1}, Generation::initial(), limits};
    EFG_CHECK_EQ(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 600, 600, PathId{1},
                                      Generation::initial(), 1))
                     .code(),
                 StatusCode::Oversized);
    EFG_CHECK_EQ(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1100, 400, PathId{1},
                                      Generation::initial(), 1))
                     .code(),
                 StatusCode::Oversized);
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 400, PathId{1},
                                      Generation::initial(), 1)));
}

EFG_TEST(evidence, completion_is_recorded_once_and_ordered) {
    FlowLedger ledger{FlowId{1}, Generation::initial(), ledger_limits()};
    EFG_CHECK_OK(ledger.append(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                      Generation::initial(), 1)));
    EFG_CHECK_STATUS_OK(ledger.mark_completed(120));
    EFG_CHECK(ledger.completed());
    EFG_CHECK_STATUS_OK(ledger.mark_completed(120));
    EFG_CHECK_EQ(ledger.mark_completed(130).code(), StatusCode::Conflict);

    FlowLedger other{FlowId{2}, Generation::initial(), ledger_limits()};
    EFG_CHECK_OK(other.append(sample(FlowId{2}, EvidenceWindowId{1}, 100, 110, 1000, 1000, PathId{1},
                                     Generation::initial(), 1)));
    EFG_CHECK_EQ(other.mark_completed(50).code(), StatusCode::ContradictoryEvidence);
}

EFG_TEST(capacity, snapshots_are_generation_bound_and_never_roll_back) {
    CapacityTable table;
    EFG_CHECK_STATUS_OK(table.submit(capacity(ResourceId{1}, 1000, 100, kStart, 1000,
                                             Generation{2}, 5)));
    EFG_CHECK_EQ(table.submit(capacity(ResourceId{1}, 1000, 100, kStart, 1000, Generation{1}, 4)).code(),
                 StatusCode::Stale);
    EFG_CHECK_STATUS_OK(table.submit(capacity(ResourceId{1}, 2000, 100, kStart + 5, 1000,
                                             Generation{2}, 6)));
    EFG_CHECK_EQ(table.find(ResourceId{1})->capacity, 2000u);
    EFG_CHECK_EQ(table.submit(capacity(ResourceId{1}, 3000, 100, kStart, 1000, Generation{2}, 7)).code(),
                 StatusCode::Stale);
}

EFG_TEST(capacity, reserved_above_total_is_structural_contradiction) {
    const ResourceCapacity invalid = capacity(ResourceId{1}, 100, 200);
    EFG_CHECK_EQ(invalid.validate().code(), StatusCode::ContradictoryEvidence);
    EFG_CHECK_EQ(invalid.available().code(), StatusCode::Underflow);
    CapacityTable table;
    EFG_CHECK_EQ(table.submit(invalid).code(), StatusCode::ContradictoryEvidence);
    EFG_CHECK_EQ(table.size(), 0u);
}

EFG_TEST(capacity, zero_capacity_and_empty_span_are_refused) {
    ResourceCapacity zero = capacity(ResourceId{1}, 0, 0);
    EFG_CHECK_EQ(zero.validate().code(), StatusCode::InvalidArgument);
    ResourceCapacity span = capacity(ResourceId{1}, 100, 0);
    span.span = TickSpan{10, 10};
    EFG_CHECK_EQ(span.validate().code(), StatusCode::InvalidArgument);
    ResourceCapacity expired = capacity(ResourceId{1}, 100, 0);
    expired.validity = ValidityWindow{kStart, kStart + 1};
    EFG_CHECK_EQ(expired.validate().code(), StatusCode::Expired);
}

EFG_TEST(capacity, expiry_sweeps_closed_snapshots) {
    CapacityTable table;
    EFG_CHECK_STATUS_OK(table.submit(capacity(ResourceId{1}, 1000, 100, kStart, 10)));
    EFG_CHECK_EQ(table.size(), 1u);
    EFG_CHECK_STATUS_OK(table.expire(kStart + 5));
    EFG_CHECK_EQ(table.size(), 1u);
    EFG_CHECK_STATUS_OK(table.expire(kStart + 10));
    EFG_CHECK_EQ(table.size(), 0u);
    EFG_CHECK(table.find(ResourceId{1}) == nullptr);
}

EFG_TEST(path, descriptors_reject_duplicate_and_absent_resources) {
    EFG_CHECK_STATUS_OK(path(PathId{1}, {ResourceId{1}, ResourceId{2}}).validate());
    EFG_CHECK_EQ(path(PathId{1}, {}).validate().code(), StatusCode::InvalidArgument);
    EFG_CHECK_EQ(path(PathId{1}, {ResourceId{1}, ResourceId{1}}).validate().code(),
                 StatusCode::ContradictoryEvidence);
    EFG_CHECK_EQ(path(PathId{1}, {ResourceId{1}, ResourceId{0}}).validate().code(),
                 StatusCode::InvalidArgument);
}

EFG_TEST(path, a_reroute_advances_the_generation_and_rejects_regression) {
    PathTable table;
    EFG_CHECK_STATUS_OK(table.submit(path(PathId{1}, {ResourceId{1}}, kStart, 1000, Generation{2})));
    EFG_CHECK_EQ(table.submit(path(PathId{1}, {ResourceId{2}}, kStart, 1000, Generation{1})).code(),
                 StatusCode::Stale);
    EFG_CHECK_STATUS_OK(table.submit(path(PathId{1}, {ResourceId{2}}, kStart + 1, 1000, Generation{3})));
    EFG_CHECK_EQ(table.find(PathId{1})->resources.size(), 1u);
    EFG_CHECK_EQ(table.find(PathId{1})->resources[0].value(), 2u);
}
