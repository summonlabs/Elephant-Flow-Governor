// Elephant Flow Governor - classification invariant tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// These tests encode the invariants the product exists to defend:
//   * stale flow, path or capacity evidence invalidates classification;
//   * UNKNOWN never classifies;
//   * protected obligations remain protected;
//   * hysteresis prevents flapping;
//   * classification history is deterministic;
//   * a flow generation change breaks old classification authority.

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace efg;
using namespace efgtest;

EFG_TEST(classification, a_sustained_high_volume_flow_qualifies) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    const Tick end = feed_constant(governor, FlowId{1}, 3000, 10, 4);

    StatusOr<FlowClassification> classification =
        governor.classification(FlowId{1}, Generation::initial(), end);
    EFG_REQUIRE(classification.ok());
    EFG_CHECK_EQ(classification.value().state, ElephantState::Elephant);
    EFG_CHECK_EQ(classification.value().authority, AuthorityLevel::Govern);
    EFG_CHECK(classification.value().authorizes_governance());
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::VolumeThresholdMet));
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::SustainedRateThresholdMet));
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::ResourceShareThresholdMet));
    EFG_CHECK_EQ(classification.value().measurements.cumulative_bytes, 120000u);
    EFG_CHECK_EQ(classification.value().measurements.share_of_capacity, 3000u);
    EFG_CHECK(classification.value().impact.severity > 0);
}

EFG_TEST(classification, a_low_volume_flow_is_definitively_not_an_elephant) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    const Tick end = feed_constant(governor, FlowId{2}, 4, 10, 4);

    StatusOr<FlowClassification> classification =
        governor.classification(FlowId{2}, Generation::initial(), end);
    EFG_REQUIRE(classification.ok());
    EFG_CHECK_EQ(classification.value().state, ElephantState::NotElephant);
    EFG_CHECK_EQ(classification.value().authority, AuthorityLevel::None);
    EFG_CHECK(!classification.value().authorizes_governance());
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::PolicyRuleNotSatisfied));
}

EFG_TEST(classification, unknown_capacity_never_classifies) {
    Governor governor;
    governor.set_incarnation(boot());
    EFG_CHECK_STATUS_OK(governor.submit_policy(default_policy()));
    EFG_CHECK_STATUS_OK(governor.submit_path(path(PathId{1}, {ResourceId{1}})));
    // No capacity snapshot is supplied, so share of resource is unknowable.
    const Tick end = feed_constant(governor, FlowId{1}, 3000, 10, 4);

    StatusOr<FlowClassification> classification =
        governor.classification(FlowId{1}, Generation::initial(), end);
    EFG_REQUIRE(classification.ok());
    EFG_CHECK_EQ(classification.value().state, ElephantState::Unknown);
    EFG_CHECK_EQ(classification.value().authority, AuthorityLevel::None);
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::CapacityMissing));
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::CapacityStale));
}

EFG_TEST(classification, an_unknown_path_never_classifies) {
    Governor governor;
    governor.set_incarnation(boot());
    EFG_CHECK_STATUS_OK(governor.submit_policy(default_policy()));
    EFG_CHECK_STATUS_OK(governor.submit_capacity(capacity(ResourceId{1}, 10000000, 1000000)));
    const Tick end = feed_constant(governor, FlowId{1}, 3000, 10, 4);

    StatusOr<FlowClassification> classification =
        governor.classification(FlowId{1}, Generation::initial(), end);
    EFG_REQUIRE(classification.ok());
    EFG_CHECK_EQ(classification.value().state, ElephantState::Unknown);
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::PathUnknown));
}

EFG_TEST(classification, stale_evidence_suspends_and_withdraws_authority) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    (void)feed_constant(governor, FlowId{1}, 3000, 10, 4, kStart, PathId{1});
    // Evidence validity is bounded: the last window closes well before this tick.
    const Tick late = kStart + 500000;

    StatusOr<FlowClassification> classification =
        governor.classification(FlowId{1}, Generation::initial(), late);
    EFG_REQUIRE(classification.ok());
    EFG_CHECK_EQ(classification.value().state, ElephantState::Suspended);
    EFG_CHECK_EQ(classification.value().authority, AuthorityLevel::None);
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::EvidenceStale));
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::RevalidationRequired));
    EFG_CHECK_EQ(governor.counters().authority_revocations, 1u);
}

EFG_TEST(classification, a_telemetry_gap_suspends_a_qualified_flow) {
    GovernorConfig config;
    config.max_telemetry_gap_ticks = 10;
    Governor governor(config);
    governor.set_incarnation(boot());
    install_standard(governor);

    WindowFeeder feeder(governor, FlowId{1}, 10);
    EFG_CHECK_EQ(qualify(feeder), ElephantState::Elephant);

    // A window far in the future leaves a hole in the telemetry stream.
    const FlowSample gapped =
        sample(FlowId{1}, EvidenceWindowId{99}, kStart + 500, kStart + 510, 1000000, 30000,
               PathId{1}, Generation::initial(), 99);
    StatusOr<FlowDecision> decision = governor.submit_evidence(gapped);
    EFG_REQUIRE(decision.ok());
    EFG_CHECK_EQ(decision.value().classification.state, ElephantState::Suspended);
    EFG_CHECK(has_reason(decision.value().classification.reasons, ReasonCode::TelemetryGap));
    EFG_CHECK_EQ(governor.counters().evidence_gaps, 1u);
}

EFG_TEST(classification, hysteresis_prevents_flapping_around_the_threshold) {
    GovernorConfig config;
    config.samples_per_flow = 2;  // a two window moving average makes the test crisp
    Governor governor(config);
    governor.set_incarnation(boot());
    EFG_CHECK_STATUS_OK(governor.submit_policy(hysteresis_policy(2, 3, 2000000, 100000)));
    EFG_CHECK_STATUS_OK(governor.submit_capacity(capacity(ResourceId{1}, 10000000, 1000000)));
    EFG_CHECK_STATUS_OK(governor.submit_path(path(PathId{1}, {ResourceId{1}})));

    WindowFeeder feeder(governor, FlowId{1}, 10);
    // The first window establishes the rate; two confirmation windows are then
    // required before the elephant state is entered.
    EFG_CHECK_EQ(feeder.push(6000), ElephantState::NotElephant);
    EFG_CHECK_EQ(feeder.push(6000), ElephantState::NotElephant);
    EFG_CHECK_EQ(feeder.push(6000), ElephantState::Elephant);
    // A single low window must not drop the classification: the exit rule has to
    // hold for three consecutive windows.
    EFG_CHECK_EQ(feeder.push(100), ElephantState::Elephant);
    EFG_CHECK_EQ(feeder.push(100), ElephantState::Elephant);
    EFG_CHECK_EQ(feeder.push(100), ElephantState::Elephant);
    EFG_CHECK_EQ(feeder.push(100), ElephantState::NotElephant);
}

EFG_TEST(classification, entering_the_elephant_state_requires_confirmation) {
    GovernorConfig config;
    config.samples_per_flow = 2;
    Governor governor(config);
    governor.set_incarnation(boot());
    EFG_CHECK_STATUS_OK(governor.submit_policy(hysteresis_policy(3, 2, 2000000, 100000)));
    EFG_CHECK_STATUS_OK(governor.submit_capacity(capacity(ResourceId{1}, 10000000, 1000000)));
    EFG_CHECK_STATUS_OK(governor.submit_path(path(PathId{1}, {ResourceId{1}})));

    WindowFeeder feeder(governor, FlowId{1}, 10);
    EFG_CHECK_EQ(feeder.push(6000), ElephantState::NotElephant);
    EFG_CHECK_EQ(feeder.push(6000), ElephantState::NotElephant);
    EFG_CHECK_EQ(feeder.push(6000), ElephantState::NotElephant);
    EFG_CHECK_EQ(feeder.push(6000), ElephantState::Elephant);
    // A single low window must not drop a qualified flow whose exit rule has not
    // yet held for the required number of consecutive windows.
    EFG_CHECK_EQ(feeder.push(100), ElephantState::Elephant);
}

EFG_TEST(classification, protected_flows_are_classified_but_never_governed) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);

    WindowFeeder feeder(governor, FlowId{5}, 10);
    const ElephantState state = qualify_protected(feeder, ProtectionState::NonPreemptible,
                                                  ServiceClass::Reserved, true);
    EFG_CHECK_EQ(state, ElephantState::Elephant);

    StatusOr<FlowClassification> classification =
        governor.classification(FlowId{5}, Generation::initial(), feeder.cursor());
    EFG_REQUIRE(classification.ok());
    EFG_CHECK_EQ(classification.value().state, ElephantState::Elephant);
    EFG_CHECK_EQ(classification.value().authority, AuthorityLevel::Observe);
    EFG_CHECK(!classification.value().authorizes_governance());
    EFG_CHECK(classification.value().is_protected());
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::ProtectedObligation));
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::NonPreemptibleObligation));
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::ReservedCapacityBounded));

    for (const GovernanceIntent& intent : governor.intents().all_intents()) {
        EFG_CHECK(intent.kind == IntentKind::ProtectReservedFlow ||
                  intent.kind == IntentKind::ObserveOnly);
    }
}

EFG_TEST(classification, an_unknown_protection_state_observes_but_never_governs) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);

    WindowFeeder feeder(governor, FlowId{6}, 10);
    const ElephantState state =
        qualify_protected(feeder, ProtectionState::Unknown, ServiceClass::Unknown, false);
    EFG_CHECK_EQ(state, ElephantState::Elephant);

    StatusOr<FlowClassification> classification =
        governor.classification(FlowId{6}, Generation::initial(), feeder.cursor());
    EFG_REQUIRE(classification.ok());
    EFG_CHECK_EQ(classification.value().authority, AuthorityLevel::Observe);
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::EvidenceUnknownField));
}

EFG_TEST(classification, a_flow_generation_rollover_breaks_old_authority) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);

    WindowFeeder first(governor, FlowId{9}, 10, kStart, PathId{1}, Generation{1});
    EFG_CHECK_EQ(qualify(first), ElephantState::Elephant);
    EFG_CHECK(governor.counters().intents_authorized > 0);
    const std::size_t live_before = governor.intents().live_count();
    EFG_CHECK(live_before > 0);

    // The same flow identifier reappears with a new generation: a rollover or an
    // identity reuse. Everything bound to generation one loses authority.
    WindowFeeder second(governor, FlowId{9}, 10, kStart + 1000, PathId{1}, Generation{2});
    EFG_CHECK_EQ(qualify(second), ElephantState::Elephant);

    EFG_CHECK_EQ(governor.counters().flow_generation_rollovers, 1u);
    EFG_CHECK(governor.counters().intents_fenced > 0);
    for (const GovernanceIntent& intent : governor.intents().for_flow(FlowId{9}, Generation{1})) {
        EFG_CHECK(!is_live(intent.state));
    }
}

EFG_TEST(classification, a_new_policy_generation_revokes_everything_it_did_not_authorize) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);

    WindowFeeder feeder(governor, FlowId{1}, 10);
    EFG_CHECK_EQ(qualify(feeder), ElephantState::Elephant);
    EFG_CHECK(governor.intents().live_count() > 0);

    PolicyDocument revised = default_policy(kStart, 100000, 4000000, 100000, 8, 300);
    revised.generation = Generation{2};
    revised.provenance.publisher.sequence = 2;
    EFG_CHECK_STATUS_OK(revised.finalize());
    EFG_CHECK_STATUS_OK(governor.submit_policy(revised));

    StatusOr<FlowClassification> classification =
        governor.classification(FlowId{1}, Generation::initial(), feeder.cursor());
    EFG_REQUIRE(classification.ok());
    EFG_CHECK_EQ(classification.value().state, ElephantState::Suspended);
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::PolicyChanged));
    EFG_CHECK_EQ(governor.intents().live_count(), 0u);
    EFG_CHECK(governor.counters().intents_revoked > 0);
}

EFG_TEST(classification, a_capacity_generation_change_suspends_classification) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);

    WindowFeeder feeder(governor, FlowId{1}, 10);
    EFG_CHECK_EQ(qualify(feeder), ElephantState::Elephant);

    EFG_CHECK_STATUS_OK(governor.submit_capacity(
        capacity(ResourceId{1}, 20000000, 1000000, kStart, 100000, Generation{2}, 2)));

    StatusOr<FlowClassification> classification =
        governor.classification(FlowId{1}, Generation::initial(), feeder.cursor());
    EFG_REQUIRE(classification.ok());
    EFG_CHECK_EQ(classification.value().state, ElephantState::Suspended);
    EFG_CHECK(has_reason(classification.value().reasons, ReasonCode::CapacityStale));
}

EFG_TEST(classification, a_reroute_suspends_then_requires_fresh_evidence) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    EFG_CHECK_STATUS_OK(governor.submit_path(path(PathId{2}, {ResourceId{1}})));

    WindowFeeder feeder(governor, FlowId{1}, 10);
    EFG_CHECK_EQ(qualify(feeder), ElephantState::Elephant);

    // The same publisher restates the generation's cumulative volume on the new
    // path: the re-route must not look like a cumulative regression.
    WindowFeeder rerouted(governor, FlowId{1}, 10, feeder.cursor(), PathId{2},
                          Generation::initial(), Generation::initial(), feeder.cumulative(),
                          feeder.sequence());
    // The first window on the new path is a new measurement epoch and withdraws
    // the authority the old path binding justified.
    EFG_CHECK_EQ(rerouted.push(3000), ElephantState::Suspended);
    // Two contiguous windows on the new path constitute a fresh measurement, and
    // the flow re-qualifies against the policy it is still governed by.
    EFG_CHECK_EQ(rerouted.push(3000), ElephantState::Elephant);
    EFG_CHECK_EQ(rerouted.push(3000), ElephantState::Elephant);
}

EFG_TEST(classification, classification_history_is_deterministic_and_bound) {
    auto run = [](u64& digest_out, u64& transitions_out, std::size_t& entries_out) {
        Governor governor;
        governor.set_incarnation(boot());
        install_standard(governor);
        WindowFeeder feeder(governor, FlowId{1}, 10);
        for (int i = 0; i < 6; ++i) {
            feeder.push(3000);
        }
        for (int i = 0; i < 6; ++i) {
            feeder.push(10);
        }
        const ClassificationHistory* history =
            governor.history(FlowId{1}, Generation::initial());
        EFG_REQUIRE(history != nullptr);
        digest_out = history->digest();
        transitions_out = governor.counters().transitions;
        entries_out = history->size();
    };

    u64 first_digest = 0;
    u64 first_transitions = 0;
    std::size_t first_entries = 0;
    u64 second_digest = 0;
    u64 second_transitions = 0;
    std::size_t second_entries = 0;
    run(first_digest, first_transitions, first_entries);
    run(second_digest, second_transitions, second_entries);
    EFG_CHECK_EQ(first_digest, second_digest);
    EFG_CHECK_EQ(first_transitions, second_transitions);
    EFG_CHECK_EQ(first_entries, second_entries);
    EFG_CHECK(first_transitions >= 2);
}

EFG_TEST(classification, two_governors_fed_identically_agree_bit_for_bit) {
    auto build = []() {
        auto governor = std::make_unique<Governor>();
        governor->set_incarnation(boot());
        install_standard(*governor);
        WindowFeeder feeder(*governor, FlowId{1}, 10);
        for (int i = 0; i < 5; ++i) {
            feeder.push(3000);
        }
        WindowFeeder second(*governor, FlowId{2}, 10);
        for (int i = 0; i < 5; ++i) {
            second.push(4);
        }
        return governor;
    };
    const std::unique_ptr<Governor> first = build();
    const std::unique_ptr<Governor> second = build();
    const StatusOr<u64> first_digest = decision_digest(*first);
    const StatusOr<u64> second_digest = decision_digest(*second);
    EFG_REQUIRE(first_digest.ok());
    EFG_REQUIRE(second_digest.ok());
    EFG_CHECK_EQ(first_digest.value(), second_digest.value());

    const StatusOr<Explanation> first_explanation =
        first->explain(FlowId{1}, Generation::initial(), kStart + 50);
    const StatusOr<Explanation> second_explanation =
        second->explain(FlowId{1}, Generation::initial(), kStart + 50);
    EFG_REQUIRE(first_explanation.ok());
    EFG_REQUIRE(second_explanation.ok());
    EFG_CHECK_EQ(render_text(first_explanation.value()),
                 render_text(second_explanation.value()));
}

EFG_TEST(classification, evidence_is_refused_without_a_policy) {
    Governor governor;
    governor.set_incarnation(boot());
    const StatusOr<FlowDecision> decision =
        governor.submit_evidence(sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000));
    EFG_CHECK_EQ(decision.code(), StatusCode::NotFound);
}

EFG_TEST(classification, a_retired_generation_cannot_be_resurrected) {
    GovernorConfig config;
    config.max_flows = 1;
    Governor governor(config);
    governor.set_incarnation(boot());
    install_standard(governor);

    EFG_CHECK_EQ(governor.complete_flow(FlowId{1}, Generation::initial(), 0).code(),
                 StatusCode::NotFound);

    WindowFeeder first(governor, FlowId{1}, 10, kStart, PathId{1}, Generation{1});
    first.push(3000);
    EFG_CHECK_STATUS_OK(governor.complete_flow(FlowId{1}, Generation::initial(), kStart + 10));

    // A second flow forces the ceiling, which retires the completed generation.
    WindowFeeder second(governor, FlowId{2}, 10, kStart, PathId{1}, Generation{1});
    second.push(3000);
    EFG_CHECK(governor.counters().flow_evictions >= 1);

    // The retired generation must not be accepted again.
    const StatusOr<FlowDecision> revived =
        governor.submit_evidence(sample(FlowId{1}, EvidenceWindowId{50}, 2000, 2010, 10, 10,
                                        PathId{1}, Generation::initial(), 50));
    EFG_CHECK_EQ(revived.code(), StatusCode::Stale);
    EFG_CHECK(governor.counters().stale_refusals > 0);
}

EFG_TEST(classification, the_population_ceiling_is_enforced_not_exceeded) {
    GovernorConfig config;
    config.max_flows = 2;
    Governor governor(config);
    governor.set_incarnation(boot());
    install_standard(governor);

    for (u64 id = 1; id <= 2; ++id) {
        WindowFeeder feeder(governor, FlowId{id}, 10);
        feeder.push(3000);
    }
    const StatusOr<FlowDecision> overflow =
        governor.submit_evidence(sample(FlowId{3}, EvidenceWindowId{1}, kStart, kStart + 10, 1000,
                                        1000, PathId{1}, Generation::initial(), 1));
    EFG_CHECK_EQ(overflow.code(), StatusCode::CapacityExceeded);
    EFG_CHECK_EQ(governor.flow_count(), 2u);
    EFG_CHECK(governor.counters().capacity_refusals >= 1);
}

EFG_TEST(classification, a_completed_flow_fences_its_governance_intent) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    WindowFeeder feeder(governor, FlowId{1}, 10);
    EFG_CHECK_EQ(qualify(feeder), ElephantState::Elephant);
    EFG_CHECK(governor.intents().live_count() > 0);

    EFG_CHECK_STATUS_OK(governor.complete_flow(FlowId{1}, Generation::initial(), feeder.cursor()));
    EFG_CHECK_EQ(governor.intents().live_count(), 0u);
    EFG_CHECK(governor.counters().intents_revoked > 0);
}

EFG_TEST(classification, the_sweep_withdraws_authority_whose_evidence_aged_out) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    WindowFeeder feeder(governor, FlowId{1}, 10);
    EFG_CHECK_EQ(qualify(feeder), ElephantState::Elephant);
    EFG_CHECK(governor.intents().live_count() > 0);

    StatusOr<DecisionBatch> batch = governor.tick(kStart + 500000);
    EFG_REQUIRE(batch.ok());
    EFG_CHECK_EQ(batch.value().suspensions, 1u);
    EFG_CHECK_EQ(governor.intents().live_count(), 0u);
    EFG_CHECK(governor.counters().intents_expired > 0 ||
              governor.counters().intents_revoked > 0);
}

EFG_TEST(classification, a_sweep_tick_may_not_regress) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    EFG_CHECK_OK(governor.tick(kStart + 100));
    EFG_CHECK_EQ(governor.tick(kStart + 50).code(), StatusCode::Stale);
    EFG_CHECK(governor.counters().stale_refusals >= 1);
}
