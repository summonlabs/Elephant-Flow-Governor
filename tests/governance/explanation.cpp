// Elephant Flow Governor - explanation tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace efg;
using namespace efgtest;

namespace {

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

EFG_TEST(explanation, a_qualified_flow_explains_why_it_qualified) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    WindowFeeder feeder(governor, FlowId{1}, 10);
    EFG_CHECK_EQ(qualify(feeder), ElephantState::Elephant);

    StatusOr<Explanation> explanation =
        governor.explain(FlowId{1}, Generation::initial(), feeder.cursor());
    EFG_REQUIRE(explanation.ok());
    EFG_CHECK(explanation.value().found);
    EFG_CHECK_EQ(explanation.value().state, ElephantState::Elephant);

    const std::string text = render_text(explanation.value());
    EFG_CHECK(contains(text, "state=elephant"));
    EFG_CHECK(contains(text, "authority=govern"));
    EFG_CHECK(contains(text, "sustained_rate_threshold_met"));
    EFG_CHECK(contains(text, "share_of_capacity"));
    EFG_CHECK(contains(text, "binding_resource=1"));
    EFG_CHECK(contains(text, "severity="));
    EFG_CHECK(contains(text, "history_digest="));
    EFG_CHECK(contains(text, "explanation_digest="));
    EFG_CHECK(contains(text, "epoch="));
    EFG_CHECK(contains(text, "boot="));
    EFG_CHECK(contains(text, "publisher="));
}

EFG_TEST(explanation, a_stale_flow_explains_the_refusal) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    WindowFeeder feeder(governor, FlowId{4}, 10);
    qualify(feeder);

    StatusOr<Explanation> explanation =
        governor.explain(FlowId{4}, Generation::initial(), kStart + 500000);
    EFG_REQUIRE(explanation.ok());
    const std::string text = render_text(explanation.value());
    EFG_CHECK(contains(text, "state=suspended"));
    EFG_CHECK(contains(text, "authority=none"));
    EFG_CHECK(contains(text, "evidence_stale"));
    EFG_CHECK(contains(text, "revalidation_required"));
}

EFG_TEST(explanation, protection_is_visible_even_when_the_flow_is_classified) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    WindowFeeder feeder(governor, FlowId{8}, 10);
    EFG_CHECK_EQ(qualify_protected(feeder, ProtectionState::NonPreemptible, ServiceClass::Reserved,
                                   true, 9000),
                 ElephantState::Elephant);

    StatusOr<Explanation> explanation =
        governor.explain(FlowId{8}, Generation::initial(), feeder.cursor());
    EFG_REQUIRE(explanation.ok());
    const std::string text = render_text(explanation.value());
    EFG_CHECK(contains(text, "protected_obligation"));
    EFG_CHECK(contains(text, "non_preemptible_obligation"));
    EFG_CHECK(contains(text, "reserved_capacity_bounded"));
    EFG_CHECK(contains(text, "authority=observe"));
    EFG_CHECK(contains(text, "protect_reserved_flow"));
    EFG_CHECK(!contains(text, "request_rate_shaping"));
}

EFG_TEST(explanation, an_unknown_flow_is_reported_as_untracked) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    const StatusOr<Explanation> explanation =
        governor.explain(FlowId{999}, Generation::initial(), kStart + 10);
    EFG_CHECK_EQ(explanation.code(), StatusCode::NotFound);
}

EFG_TEST(explanation, json_rendering_is_well_formed_and_stable) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    WindowFeeder feeder(governor, FlowId{1}, 10);
    qualify(feeder);

    StatusOr<Explanation> explanation =
        governor.explain(FlowId{1}, Generation::initial(), feeder.cursor());
    EFG_REQUIRE(explanation.ok());
    const std::string json = render_json(explanation.value());
    EFG_CHECK(contains(json, "\"state\": \"elephant\""));
    EFG_CHECK(contains(json, "\"authority\": \"govern\""));
    EFG_CHECK(contains(json, "\"measurements\""));
    EFG_CHECK(contains(json, "\"thresholds\""));
    EFG_CHECK(contains(json, "\"impact\""));
    EFG_CHECK(contains(json, "\"authority_vector\""));
    EFG_CHECK(contains(json, "\"intents\""));
    EFG_CHECK(contains(json, "explanation_digest"));

    // Balanced braces and brackets are a cheap structural sanity check.
    int depth = 0;
    int brackets = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (in_string) {
            if (c == static_cast<char>(92)) {
                ++i;
            } else if (c == static_cast<char>(34)) {
                in_string = false;
            }
            continue;
        }
        if (c == static_cast<char>(34)) {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
        } else if (c == '[') {
            ++brackets;
        } else if (c == ']') {
            --brackets;
        }
    }
    EFG_CHECK_EQ(depth, 0);
    EFG_CHECK_EQ(brackets, 0);
    EFG_CHECK(!in_string);
}

EFG_TEST(explanation, the_rendered_explanation_is_bounded) {
    Governor governor;
    governor.set_incarnation(boot());
    PolicyDocument policy = default_policy(kStart, 100000, 1000, 1000, 1, 1);
    EFG_CHECK_STATUS_OK(policy.finalize());
    EFG_CHECK_STATUS_OK(governor.submit_policy(policy));
    EFG_CHECK_STATUS_OK(governor.submit_capacity(capacity(ResourceId{1}, 10000000, 1000000)));
    EFG_CHECK_STATUS_OK(governor.submit_path(path(PathId{1}, {ResourceId{1}})));

    WindowFeeder feeder(governor, FlowId{1}, 10);
    qualify(feeder);
    StatusOr<Explanation> explanation =
        governor.explain(FlowId{1}, Generation::initial(), feeder.cursor());
    EFG_REQUIRE(explanation.ok());
    const std::string text = render_text(explanation.value());
    EFG_CHECK(text.size() <= limits::kMaxExplanationText + 512);

    bool truncated = false;
    const std::vector<ExplanationLine> lines =
        build_explanation_lines(explanation.value(), truncated);
    EFG_CHECK(lines.size() <= limits::kMaxExplanationLines);
    for (const ExplanationLine& line : lines) {
        EFG_CHECK(!line.text.empty());
    }
}

EFG_TEST(explanation, identical_inputs_produce_identical_explanations) {
    auto build = [](std::string& text, std::string& json, u64& digest) {
        Governor governor;
        governor.set_incarnation(boot(5, 6));
        install_standard(governor);
        WindowFeeder feeder(governor, FlowId{2}, 10);
        qualify(feeder);
        feeder.push(4);
        StatusOr<Explanation> explanation =
            governor.explain(FlowId{2}, Generation::initial(), feeder.cursor());
        EFG_REQUIRE(explanation.ok());
        text = render_text(explanation.value());
        json = render_json(explanation.value());
        digest = explanation.value().digest();
    };
    std::string first_text;
    std::string first_json;
    u64 first_digest = 0;
    std::string second_text;
    std::string second_json;
    u64 second_digest = 0;
    build(first_text, first_json, first_digest);
    build(second_text, second_json, second_digest);
    EFG_CHECK_EQ(first_text, second_text);
    EFG_CHECK_EQ(first_json, second_json);
    EFG_CHECK_EQ(first_digest, second_digest);
}

EFG_TEST(explanation, the_authority_vector_names_every_binding) {
    AuthorityVector authority;
    authority.flow = FlowId{1};
    authority.flow_generation = Generation{2};
    authority.path = PathId{3};
    authority.path_generation = Generation{4};
    authority.capacity = CapacitySnapshotId{5};
    authority.capacity_generation = Generation{6};
    authority.window = EvidenceWindowId{7};
    authority.window_sequence = 8;
    authority.policy = PolicyId{9};
    authority.policy_generation = Generation{10};
    authority.classification_generation = Generation{11};
    authority.epoch = EpochId{12};
    authority.boot = BootId{13};
    authority.publisher = PublisherId{14};
    authority.publisher_boot = BootId{15};
    authority.bound_flags = 0xFFu;

    const std::string rendered = render_authority_vector(authority);
    for (const char* token : {"flow=1", "path=3", "capacity=5", "window=7", "policy=9",
                             "epoch=12", "boot=13", "publisher=14"}) {
        EFG_CHECK(contains(rendered, token));
    }
}
