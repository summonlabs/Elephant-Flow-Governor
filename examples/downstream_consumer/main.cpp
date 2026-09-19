// Elephant Flow Governor - independent downstream consumer.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This program is a separate project. It is built only from the installed
// package, includes only public headers, and exercises the contract the library
// advertises: submit evidence, read a classification, read the explanation, read
// the bounded governance intent.

#include <cstdio>
#include <string>
#include <vector>

#include <efg/efg.hpp>

namespace {

constexpr efg::Tick kStart = 1000;

efg::Provenance provenance(std::uint64_t sequence, efg::Tick at) {
    efg::Provenance out;
    out.publisher.publisher = efg::PublisherId{1};
    out.publisher.boot = efg::BootId{1};
    out.publisher.epoch = efg::EpochId{1};
    out.publisher.sequence = sequence;
    out.emitted_at = at;
    out.schema_version = efg::kSchemaVersion;
    return out;
}

efg::PolicyDocument build_policy() {
    using namespace efg;
    PolicyDocument policy;
    policy.id = PolicyId{1};
    policy.generation = Generation::initial();

    RuleSet rules;
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::CumulativeBytes, 200000,
                                             ServiceClass::Unknown},
                                   {}});
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::DurationTicks, 16, ServiceClass::Unknown},
                                   {}});
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::SustainedRate, 4000000,
                                             ServiceClass::Unknown},
                                   {}});
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::ShareOfCapacity, 1500,
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
    policy.validity = ValidityWindow{kStart, kStart + 100000};
    policy.provenance = provenance(1, kStart);
    (void)policy.finalize();
    return policy;
}

}  // namespace

int main() {
    using namespace efg;

    Governor governor;
    BootIdentity boot;
    boot.boot = BootId{1};
    boot.epoch = EpochId{1};
    boot.started_at = kStart;
    boot.monotonic_seed = 1;
    governor.set_incarnation(boot);

    Status status = governor.submit_policy(build_policy());
    if (!status.ok()) {
        std::fprintf(stderr, "policy rejected: %s\n", status.to_string().c_str());
        return 2;
    }

    ResourceCapacity capacity;
    capacity.resource = ResourceId{1};
    capacity.generation = Generation::initial();
    capacity.snapshot = CapacitySnapshotId{1};
    capacity.span = TickSpan{kStart, kStart + 64};
    capacity.capacity = 10000000;
    capacity.reserved = 1000000;
    capacity.validity = ValidityWindow{kStart, kStart + 100000};
    capacity.provenance = provenance(2, kStart);
    status = governor.submit_capacity(capacity);
    if (!status.ok()) {
        std::fprintf(stderr, "capacity rejected: %s\n", status.to_string().c_str());
        return 2;
    }

    PathDescriptor path;
    path.path = PathId{1};
    path.generation = Generation::initial();
    path.resources = {ResourceId{1}};
    path.validity = ValidityWindow{kStart, kStart + 100000};
    path.provenance = provenance(3, kStart);
    status = governor.submit_path(path);
    if (!status.ok()) {
        std::fprintf(stderr, "path rejected: %s\n", status.to_string().c_str());
        return 2;
    }

    u64 cumulative = 0;
    ElephantState state = ElephantState::Unknown;
    for (u64 window = 1; window <= 6; ++window) {
        const Tick begin = kStart + (window - 1) * 10;
        const u64 window_bytes = 90000;
        cumulative += window_bytes;

        FlowSample sample;
        sample.flow = FlowId{1};
        sample.flow_generation = Generation::initial();
        sample.path = PathId{1};
        sample.path_generation = Generation::initial();
        sample.window = EvidenceWindowId{window};
        sample.window_sequence = window;
        sample.span = TickSpan{begin, begin + 10};
        sample.cumulative_bytes = cumulative;
        sample.window_bytes = window_bytes;
        sample.service_class = ServiceClass::Standard;
        sample.priority = Priority{3};
        sample.protection = ProtectionState::Unprotected;
        sample.validity = ValidityWindow{begin, begin + 100000};
        sample.provenance = provenance(window + 3, begin);
        sample.provenance.digest = sample.digest();

        StatusOr<FlowDecision> decision = governor.submit_evidence(sample);
        if (!decision.ok()) {
            std::fprintf(stderr, "evidence rejected: %s\n", decision.status().to_string().c_str());
            return 3;
        }
        state = decision.value().classification.state;
    }

    if (state != ElephantState::Elephant) {
        std::fprintf(stderr, "the consumer expected an elephant classification\n");
        return 4;
    }

    StatusOr<Explanation> explanation =
        governor.explain(FlowId{1}, Generation::initial(), kStart + 60);
    if (!explanation.ok()) {
        std::fprintf(stderr, "explanation failed: %s\n", explanation.status().to_string().c_str());
        return 5;
    }
    if (!explanation.value().authority_vector.binds(kAuthorityFlowBound) ||
        !explanation.value().authority_vector.binds(kAuthorityPolicyBound)) {
        std::fprintf(stderr, "the authority vector is incomplete\n");
        return 6;
    }

    std::printf("library_version=%s\n", std::string{version_string()}.c_str());
    std::printf("state=%s authority=%s\n", std::string{to_string(state)}.c_str(),
                std::string{to_string(explanation.value().authority)}.c_str());
    std::printf("flows=%zu intents=%zu\n", governor.flow_count(), governor.intents().size());

    const std::string rendered = render_text(explanation.value());
    if (rendered.find("explanation_digest=") == std::string::npos) {
        std::fprintf(stderr, "the rendered explanation is missing its digest\n");
        return 7;
    }
    std::fputs(rendered.c_str(), stdout);

    const StatusOr<u64> digest = decision_digest(governor);
    if (!digest.ok()) {
        return 8;
    }
    std::printf("decision_digest=%llu\n", static_cast<unsigned long long>(digest.value()));
    return 0;
}
