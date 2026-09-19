// Elephant Flow Governor - synthetic population benchmark tool.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

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

    const PopulationSpec spec = population_from_args(args);
    GovernorConfig config;
    config.max_flows = args.get_size("max-flows", 65536);
    config.samples_per_flow = args.get_size("samples-per-flow", 8);
    config.classification_validity_ticks = args.get_u64("classification-validity", 512);
    config.intent_duration_ticks = args.get_u64("intent-duration", 64);

    StatusOr<BenchmarkResult> result = run_benchmark(spec, config);
    if (!result.ok()) {
        std::fprintf(stderr, "benchmark failed: %s\n", result.status().to_string().c_str());
        return 2;
    }

    const std::string json_path = args.get("json");
    if (!json_path.empty()) {
        const Status written = write_text_file(json_path, render_benchmark_json(result.value()));
        if (!written.ok()) {
            std::fprintf(stderr, "report could not be written: %s\n", written.to_string().c_str());
            return 3;
        }
    }
    std::fputs(render_benchmark_text(result.value()).c_str(), stdout);
    return result.value().deterministic_replay ? 0 : 4;
}
