// Elephant Flow Governor - cluster coordinator process.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Owns the authoritative governor and an epoch. Workers publish generation bound
// evidence over loopback framing; anything carrying a stale epoch, a foreign boot
// or an already consumed session nonce is fenced and the session is closed.

#include <cstdio>
#include <string>

#include "cli.hpp"

int main(int argc, char** argv) {
    using namespace efg;
    using namespace efg::cli;

    const Args args(argc, argv);
    if (args.has("help")) {
        std::fputs(Args::usage().c_str(), stdout);
        return 0;
    }

    const ScenarioSpec spec = scenario_from_args(args);
    const Status spec_status = spec.validate();
    if (!spec_status.ok()) {
        std::fprintf(stderr, "scenario is invalid: %s\n", spec_status.to_string().c_str());
        return 2;
    }

    const Tick start = 1000;
    BootIdentity boot;
    boot.boot = BootId{args.get_u64("boot", 1)};
    boot.epoch = EpochId{args.get_u64("epoch", 1)};
    boot.started_at = start;
    boot.monotonic_seed = spec.population.seed;

    StatusOr<PolicyDocument> policy = make_benchmark_policy(
        spec.population, spec.policy_id, spec.policy_generation, start, spec.policy_validity_ticks);
    if (!policy.ok()) {
        std::fprintf(stderr, "policy could not be built: %s\n", policy.status().to_string().c_str());
        return 2;
    }

    CoordinatorConfig config;
    config.boot = boot;
    config.start_tick = start;
    config.policy = policy.value();
    config.capacities = build_capacities(spec, start, boot);
    config.paths = build_paths(spec, start, boot);
    config.expected_sessions = args.get_size("workers", 1);
    config.limits.max_batch = args.get_size("max-batch", limits::kMaxEvidenceBatch);
    config.governor.max_flows = args.get_size("max-flows", 65536);
    config.governor.samples_per_flow = args.get_size("samples-per-flow", 8);
    config.governor.classification_validity_ticks = args.get_u64("classification-validity", 512);
    config.governor.intent_duration_ticks = args.get_u64("intent-duration", 64);
    const std::string store = args.get("store");
    if (!store.empty()) {
        config.store_directory = store;
    }

    StatusOr<std::unique_ptr<Coordinator>> coordinator = Coordinator::create(config);
    if (!coordinator.ok()) {
        std::fprintf(stderr, "coordinator could not start: %s\n",
                     coordinator.status().to_string().c_str());
        return 3;
    }
    Status status = coordinator.value()->listen();
    if (!status.ok()) {
        std::fprintf(stderr, "coordinator could not listen: %s\n", status.to_string().c_str());
        return 3;
    }
    std::printf("coordinator_epoch=%llu port=%u\n",
                static_cast<unsigned long long>(boot.epoch.value()),
                static_cast<unsigned>(coordinator.value()->port()));
    std::fflush(stdout);

    const std::string port_file = args.get("port-file");
    if (!port_file.empty()) {
        std::string text;
        append_u64(text, coordinator.value()->port());
        text.push_back('\n');
        status = write_text_file(port_file, text);
        if (!status.ok()) {
            std::fprintf(stderr, "port file could not be written: %s\n", status.to_string().c_str());
            return 3;
        }
    }

    status = coordinator.value()->run();
    if (!status.ok()) {
        std::fprintf(stderr, "coordinator failed: %s\n", status.to_string().c_str());
        return 4;
    }

    // A final sweep withdraws every classification whose evidence aged out while
    // the sessions were being served: this is how publisher death becomes a
    // revalidation rather than a silent continuation.
    if (args.has("final-tick")) {
        const Tick final_tick = args.get_u64("final-tick", start + spec.population.ticks + 1);
        StatusOr<DecisionBatch> batch = coordinator.value()->governor().tick(final_tick);
        if (!batch.ok()) {
            std::fprintf(stderr, "final sweep failed: %s\n", batch.status().to_string().c_str());
            return 4;
        }
        std::printf("final_sweep_suspensions=%llu\n",
                    static_cast<unsigned long long>(batch.value().suspensions));
        std::fflush(stdout);
    }

    const std::string report_path = args.get("report");
    if (!report_path.empty()) {
        status = coordinator.value()->write_report(report_path);
        if (!status.ok()) {
            std::fprintf(stderr, "report could not be written: %s\n", status.to_string().c_str());
            return 5;
        }
    }
    std::fputs(coordinator.value()->render_report().c_str(), stdout);

    const std::string done_path = args.get("done-file");
    if (!done_path.empty()) {
        std::string text{"done\n"};
        const Status written = write_text_file(done_path, text);
        if (!written.ok()) {
            return 5;
        }
    }
    return 0;
}
