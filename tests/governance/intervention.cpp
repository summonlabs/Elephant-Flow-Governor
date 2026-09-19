// Elephant Flow Governor - bounded governance intent tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace efg;
using namespace efgtest;

namespace {

GovernanceIntent make_intent(AttemptId attempt, IntentKind kind, Tick begin, Tick end) {
    GovernanceIntent intent;
    intent.attempt = attempt;
    intent.kind = kind;
    intent.flow = FlowId{1};
    intent.flow_generation = Generation::initial();
    intent.classification = ClassificationId{1};
    intent.classification_generation = Generation::initial();
    intent.state = IntentState::Authorized;
    intent.requested.rate_reduction_bp = 1000;
    intent.requested.duration_ticks = 50;
    intent.granted = intent.requested;
    intent.window = TickSpan{begin, end};
    intent.authority.epoch = EpochId{1};
    intent.authority.boot = BootId{1};
    intent.authority.bound_flags = kAuthorityEpochBound | kAuthorityBootBound;
    return intent;
}

}  // namespace

EFG_TEST(intent, recording_is_idempotent_on_the_attempt_identifier) {
    IntentLedger ledger;
    const GovernanceIntent intent = make_intent(AttemptId{7}, IntentKind::RequestRateShaping, 100, 200);
    StatusOr<GovernanceIntent> first = ledger.record(intent);
    EFG_REQUIRE(first.ok());
    EFG_CHECK(first.value().id.valid());
    StatusOr<GovernanceIntent> second = ledger.record(intent);
    EFG_REQUIRE(second.ok());
    EFG_CHECK_EQ(first.value().id.value(), second.value().id.value());
    EFG_CHECK_EQ(ledger.size(), 1u);
    EFG_CHECK_EQ(ledger.counters().duplicates, 1u);
    EFG_CHECK_EQ(ledger.counters().conflicts, 0u);
}

EFG_TEST(intent, reusing_an_attempt_identifier_with_different_content_is_a_conflict) {
    IntentLedger ledger;
    EFG_CHECK_OK(ledger.record(make_intent(AttemptId{7}, IntentKind::RequestRateShaping, 100, 200)));
    GovernanceIntent conflicting = make_intent(AttemptId{7}, IntentKind::EscalateCongestion, 100, 200);
    EFG_CHECK_EQ(ledger.record(conflicting).code(), StatusCode::Conflict);
    EFG_CHECK_EQ(ledger.counters().conflicts, 1u);
    EFG_CHECK_EQ(ledger.size(), 1u);
}

EFG_TEST(intent, malformed_intents_are_refused_before_they_are_stored) {
    IntentLedger ledger;
    GovernanceIntent no_attempt = make_intent(AttemptId{}, IntentKind::RequestRateShaping, 100, 200);
    EFG_CHECK_EQ(ledger.record(no_attempt).code(), StatusCode::InvalidArgument);

    GovernanceIntent no_flow = make_intent(AttemptId{1}, IntentKind::RequestRateShaping, 100, 200);
    no_flow.flow = FlowId{};
    EFG_CHECK_EQ(ledger.record(no_flow).code(), StatusCode::InvalidArgument);

    GovernanceIntent empty_window =
        make_intent(AttemptId{2}, IntentKind::RequestRateShaping, 200, 200);
    EFG_CHECK_EQ(ledger.record(empty_window).code(), StatusCode::InvalidArgument);

    GovernanceIntent bad_kind = make_intent(AttemptId{3}, IntentKind::Count, 100, 200);
    EFG_CHECK_EQ(ledger.record(bad_kind).code(), StatusCode::InvalidArgument);

    EFG_CHECK_EQ(ledger.size(), 0u);
}

EFG_TEST(intent, the_ledger_budget_is_bounded_and_refuses_rather_than_growing) {
    IntentLedger ledger{IntentLedger::Limits{2, 2}};
    EFG_CHECK_OK(ledger.record(make_intent(AttemptId{1}, IntentKind::RequestRateShaping, 100, 200)));
    EFG_CHECK_OK(ledger.record(make_intent(AttemptId{2}, IntentKind::RequestRateShaping, 100, 200)));
    EFG_CHECK_EQ(
        ledger.record(make_intent(AttemptId{3}, IntentKind::RequestRateShaping, 100, 200)).code(),
        StatusCode::CapacityExceeded);
    EFG_CHECK_EQ(ledger.size(), 2u);
    EFG_CHECK_EQ(ledger.counters().rejected_budget, 1u);
}

EFG_TEST(intent, bounds_narrow_but_never_widen) {
    IntentBounds ceiling;
    ceiling.rate_reduction_bp = 2500;
    ceiling.duration_ticks = 100;
    ceiling.targets = 4;
    ceiling.placement = true;

    IntentBounds requested;
    requested.rate_reduction_bp = 9000;
    requested.duration_ticks = 5000;
    requested.targets = 100;
    requested.placement = true;
    requested.isolation = true;

    const IntentBounds granted = narrow_bounds(requested, ceiling);
    EFG_CHECK_EQ(granted.rate_reduction_bp, 2500u);
    EFG_CHECK_EQ(granted.duration_ticks, 100u);
    EFG_CHECK_EQ(granted.targets, 4u);
    EFG_CHECK(granted.placement);
    EFG_CHECK(!granted.isolation);
    EFG_CHECK(!bounds_within(requested, ceiling));

    IntentBounds modest;
    modest.rate_reduction_bp = 100;
    modest.duration_ticks = 10;
    modest.targets = 1;
    modest.placement = true;
    EFG_CHECK(bounds_within(modest, ceiling));
}

EFG_TEST(intent, revocation_and_fencing_are_separate_states) {
    IntentLedger ledger;
    EFG_CHECK_OK(ledger.record(make_intent(AttemptId{1}, IntentKind::RequestRateShaping, 100, 200)));
    EFG_CHECK_OK(ledger.record(make_intent(AttemptId{2}, IntentKind::RequestRateShaping, 100, 200)));
    EFG_CHECK_EQ(ledger.live_count(), 2u);

    u64 revoked = 0;
    EFG_CHECK_STATUS_OK(ledger.revoke_flow(FlowId{1}, Generation::initial(), 150, revoked));
    EFG_CHECK_EQ(revoked, 2u);
    EFG_CHECK_EQ(ledger.live_count(), 0u);

    EFG_CHECK_OK(ledger.record(make_intent(AttemptId{3}, IntentKind::RequestRateShaping, 100, 200)));
    u64 fenced = 0;
    EFG_CHECK_STATUS_OK(ledger.fence_epoch(EpochId{2}, 150, fenced));
    EFG_CHECK_EQ(fenced, 1u);
    EFG_CHECK_EQ(ledger.live_count(), 0u);
    for (const GovernanceIntent& intent : ledger.all_intents()) {
        EFG_CHECK(intent.state == IntentState::Revoked || intent.state == IntentState::Fenced);
    }
}

EFG_TEST(intent, an_intent_whose_window_closed_expires) {
    IntentLedger ledger;
    EFG_CHECK_OK(ledger.record(make_intent(AttemptId{1}, IntentKind::RequestRateShaping, 100, 200)));
    EFG_CHECK_EQ(ledger.all_intents()[0].live_at(150), true);
    u64 expired = 0;
    EFG_CHECK_STATUS_OK(ledger.expire(199, expired));
    EFG_CHECK_EQ(expired, 0u);
    EFG_CHECK_STATUS_OK(ledger.expire(200, expired));
    EFG_CHECK_EQ(expired, 1u);
    EFG_CHECK_EQ(ledger.live_count(), 0u);
}

EFG_TEST(intent, a_stale_classification_generation_revokes_its_intents) {
    IntentLedger ledger;
    EFG_CHECK_OK(ledger.record(make_intent(AttemptId{1}, IntentKind::RequestRateShaping, 100, 200)));
    u64 changed = 0;
    EFG_CHECK_STATUS_OK(
        ledger.revoke_stale_generation(FlowId{1}, Generation::initial(), Generation{2}, 150, changed));
    EFG_CHECK_EQ(changed, 1u);
    EFG_CHECK_EQ(ledger.live_count(), 0u);
    EFG_CHECK_OK(ledger.record(make_intent(AttemptId{2}, IntentKind::RequestRateShaping, 100, 200)));
    EFG_CHECK_STATUS_OK(
        ledger.revoke_stale_generation(FlowId{1}, Generation::initial(), Generation::initial(), 150,
                                       changed));
    EFG_CHECK_EQ(changed, 0u);
    EFG_CHECK_EQ(ledger.live_count(), 1u);
}

EFG_TEST(intent, transition_to_an_unknown_intent_identifier_is_not_found) {
    IntentLedger ledger;
    EFG_CHECK_EQ(ledger.transition(IntentId{99}, IntentState::Revoked, SuppressReason::NoAuthority, 10)
                     .code(),
                 StatusCode::NotFound);
    EFG_CHECK(ledger.find(IntentId{99}) == nullptr);
}

EFG_TEST(intent, the_governor_suppresses_corrective_intent_without_capacity_evidence) {
    Governor governor;
    governor.set_incarnation(boot());
    EFG_CHECK_STATUS_OK(governor.submit_policy(default_policy()));
    EFG_CHECK_STATUS_OK(governor.submit_path(path(PathId{1}, {ResourceId{1}})));
    WindowFeeder feeder(governor, FlowId{1}, 10);
    qualify(feeder);

    for (const GovernanceIntent& intent : governor.intents().all_intents()) {
        EFG_CHECK(!is_corrective(intent.kind) || intent.state == IntentState::Suppressed);
        EFG_CHECK(intent.granted.rate_reduction_bp == 0 || !is_corrective(intent.kind));
    }
}

EFG_TEST(intent, a_protected_flow_never_receives_a_corrective_intent) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    WindowFeeder feeder(governor, FlowId{3}, 10);
    EFG_CHECK_EQ(qualify_protected(feeder, ProtectionState::NonPreemptible, ServiceClass::Reserved,
                                   true, 9000),
                 ElephantState::Elephant);

    bool saw_protection = false;
    for (const GovernanceIntent& intent : governor.intents().all_intents()) {
        EFG_CHECK(!is_corrective(intent.kind));
        if (intent.kind == IntentKind::ProtectReservedFlow) {
            saw_protection = true;
            EFG_CHECK_EQ(intent.state, IntentState::Authorized);
        }
    }
    EFG_CHECK(saw_protection);
}

EFG_TEST(intent, granted_bounds_never_exceed_the_policy_ceiling) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    // Two elephants share one resource, so the fair share forces a real request.
    WindowFeeder first(governor, FlowId{1}, 10, kStart, PathId{1}, Generation::initial());
    WindowFeeder second(governor, FlowId{2}, 10, kStart, PathId{1}, Generation::initial());
    qualify(first, 9000);
    qualify(second, 9000);

    const PolicyDocument* policy = governor.policy();
    EFG_REQUIRE(policy != nullptr);
    std::size_t checked = 0;
    for (const GovernanceIntent& intent : governor.intents().all_intents()) {
        EFG_CHECK(intent.granted.rate_reduction_bp <= policy->authorization.max_shaping_reduction_bp);
        EFG_CHECK(intent.granted.targets <= policy->authorization.max_targets);
        EFG_CHECK(intent.granted.duration_ticks <= policy->authorization.max_intent_duration_ticks);
        if (intent.requested.rate_reduction_bp > policy->authorization.max_shaping_reduction_bp) {
            EFG_CHECK_EQ(intent.state, IntentState::Clamped);
        }
        ++checked;
    }
    EFG_CHECK(checked > 0);
}

EFG_TEST(intent, a_live_intent_is_not_reissued_for_the_same_classification_generation) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    WindowFeeder feeder(governor, FlowId{1}, 10);
    qualify(feeder);
    const std::size_t after_qualification = governor.intents().size();
    EFG_CHECK(after_qualification > 0);
    // More windows of the same classification generation must not multiply intent.
    feeder.push(9000);
    feeder.push(9000);
    EFG_CHECK(governor.intents().size() <= after_qualification + 4);
    EFG_CHECK(governor.intents().size() < 32);
}
