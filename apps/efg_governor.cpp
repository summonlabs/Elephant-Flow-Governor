// Elephant Flow Governor - single process scenario runner.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Drives one synthetic population through the governor, sweeps the horizon and
// prints the decision, the explanations and the governance intent that resulted.
// The population is SYNTHETIC; nothing here touches a network.

#include <cstdio>
#include <string>

#include "cli.hpp"

namespace {

int run(int argc, char** argv) {
    using namespace efg;
    using namespace efg::cli;

    const Args args(argc, argv);
    if (args.has("help")) {
        std::fputs(Args::usage().c_str(), stdout);
        return 0;
    }

    ScenarioSpec spec = scenario_from_args(args);
    Status validated = spec.validate();
    if (!validated.ok()) {
        std::fprintf(stderr, "scenario is invalid: %s\n", validated.to_string().c_str());
        return 2;
    }

    const Tick start = 1000;
    GovernorConfig config;
    config.max_flows = args.get_size("max-flows", 65536);
    config.samples_per_flow = args.get_size("samples-per-flow", 8);
    config.max_telemetry_gap_ticks = args.get_u64("max-gap", 100);
    config.classification_validity_ticks = args.get_u64("classification-validity", 512);
    config.intent_duration_ticks = args.get_u64("intent-duration", 64);
    config.max_history_per_flow = args.get_size("history", limits::kMaxHistoryPerFlow);

    Governor governor(config);
    BootIdentity boot;
    boot.boot = BootId{args.get_u64("boot", 1)};
    boot.epoch = EpochId{args.get_u64("epoch", 1)};
    boot.started_at = start;
    boot.monotonic_seed = spec.population.seed;
    governor.set_incarnation(boot);

    StatusOr<PolicyDocument> policy = make_benchmark_policy(
        spec.population, spec.policy_id, spec.policy_generation, start, spec.policy_validity_ticks);
    if (!policy.ok()) {
        std::fprintf(stderr, "policy could not be built: %s\n", policy.status().to_string().c_str());
        return 2;
    }
    Status status = governor.submit_policy(policy.value());
    if (!status.ok()) {
        std::fprintf(stderr, "policy rejected: %s\n", status.to_string().c_str());
        return 3;
    }
    for (const ResourceCapacity& capacity : build_capacities(spec, start, boot)) {
        status = governor.submit_capacity(capacity);
        if (!status.ok()) {
            std::fprintf(stderr, "capacity rejected: %s\n", status.to_string().c_str());
            return 3;
        }
    }
    for (const PathDescriptor& path : build_paths(spec, start, boot)) {
        status = governor.submit_path(path);
        if (!status.ok()) {
            std::fprintf(stderr, "path rejected: %s\n", status.to_string().c_str());
            return 3;
        }
    }

    StatusOr<SampleStream> stream = build_stream(spec, boot, PublisherId{1}, 0, 1);
    if (!stream.ok()) {
        std::fprintf(stderr, "stream could not be built: %s\n", stream.status().to_string().c_str());
        return 2;
    }
    for (const FlowSample& sample : stream.value().samples) {
        StatusOr<FlowDecision> decision = governor.submit_evidence(sample);
        if (!decision.ok()) {
            std::fprintf(stderr, "evidence rejected: %s\n", decision.status().to_string().c_str());
            return 4;
        }
    }

    const Tick final_tick = start + spec.population.ticks + 1;
    StatusOr<DecisionBatch> batch = governor.tick(final_tick);
    if (!batch.ok()) {
        std::fprintf(stderr, "sweep failed: %s\n", batch.status().to_string().c_str());
        return 4;
    }

    const GovernorCounters& counters = governor.counters();
    std::string summary;
    summary.append("population: SYNTHETIC\n");
    summary.append("flows=");
    append_u64(summary, static_cast<u64>(governor.flow_count()));
    summary.append(" evidence_accepted=");
    append_u64(summary, counters.evidence_accepted);
    summary.append(" evidence_rejected=");
    append_u64(summary, counters.evidence_rejected);
    summary.append(" decisions=");
    append_u64(summary, counters.decisions);
    summary.append(" elephant=");
    append_u64(summary, counters.elephant_decisions);
    summary.append(" not_elephant=");
    append_u64(summary, counters.not_elephant_decisions);
    summary.append(" unknown=");
    append_u64(summary, counters.unknown_decisions);
    summary.append(" suspended=");
    summary.append(std::to_string(counters.suspended_decisions));
    summary.push_back('\n');
    summary.append("intents_authorized=");
    append_u64(summary, counters.intents_authorized);
    summary.append(" intents_clamped=");
    append_u64(summary, counters.intents_clamped);
    summary.append(" intents_suppressed=");
    append_u64(summary, counters.intents_suppressed);
    summary.append(" intents_fenced=");
    append_u64(summary, counters.intents_fenced);
    summary.append(" intents_expired=");
    append_u64(summary, counters.intents_expired);
    summary.push_back('\n');
    std::fputs(summary.c_str(), stdout);

    const std::size_t explain_limit = args.get_size("explain", 3);
    const std::vector<std::pair<FlowId, Generation>> keys = governor.flow_keys();
    std::size_t explained = 0;
    for (const auto& key : keys) {
        if (explained >= explain_limit) {
            break;
        }
        StatusOr<Explanation> explanation = governor.explain(key.first, key.second, final_tick);
        if (!explanation.ok()) {
            continue;
        }
        if (explanation.value().state != ElephantState::Elephant) {
            continue;
        }
        std::fputs(render_text(explanation.value()).c_str(), stdout);
        ++explained;
    }

    const StatusOr<u64> digest = decision_digest(governor);
    std::string tail{"decision_digest="};
    append_u64(tail, digest.ok() ? digest.value() : 0);
    tail.push_back('\n');
    std::fputs(tail.c_str(), stdout);

    const std::string report_path = args.get("report");
    if (!report_path.empty()) {
        std::string report = render_benchmark_json(BenchmarkResult{});
        report.clear();
        report.append("{\n  \"population_kind\": \"SYNTHETIC\",\n  \"flows\": ");
        append_u64(report, static_cast<u64>(governor.flow_count()));
        report.append(",\n  \"decisions\": ");
        append_u64(report, counters.decisions);
        report.append(",\n  \"elephant_decisions\": ");
        append_u64(report, counters.elephant_decisions);
        report.append(",\n  \"suspended_decisions\": ");
        append_u64(report, counters.suspended_decisions);
        report.append(",\n  \"intents_authorized\": ");
        append_u64(report, counters.intents_authorized);
        report.append(",\n  \"intents_clamped\": ");
        append_u64(report, counters.intents_clamped);
        report.append(",\n  \"intents_suppressed\": ");
        append_u64(report, counters.intents_suppressed);
        report.append(",\n  \"intents_fenced\": ");
        append_u64(report, counters.intents_fenced);
        report.append(",\n  \"decision_digest\": ");
        append_u64(report, digest.ok() ? digest.value() : 0);
        report.append("\n}\n");
        const Status written = write_text_file(report_path, report);
        if (!written.ok()) {
            std::fprintf(stderr, "report could not be written: %s\n", written.to_string().c_str());
            return 5;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) { return run(argc, argv); }
