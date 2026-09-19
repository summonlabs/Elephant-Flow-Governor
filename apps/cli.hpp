// Elephant Flow Governor - shared command line support.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The tools are thin: they parse arguments, build a synthetic or supplied
// scenario, drive the library and print a deterministic report. No product
// decision lives here.

#ifndef EFG_APPS_CLI_HPP
#define EFG_APPS_CLI_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "efg/benchmark.hpp"
#include "efg/capacity.hpp"
#include "efg/cluster.hpp"
#include "efg/efg.hpp"
#include "efg/path.hpp"
#include "efg/policy.hpp"

namespace efg::cli {

/// Minimal positional-free argument reader: every option is --name value.
class Args {
public:
    Args(int argc, char** argv);

    [[nodiscard]] bool has(std::string_view name) const;
    [[nodiscard]] std::string get(std::string_view name, std::string fallback = {}) const;
    [[nodiscard]] std::uint64_t get_u64(std::string_view name, std::uint64_t fallback) const;
    [[nodiscard]] std::size_t get_size(std::string_view name, std::size_t fallback) const;
    [[nodiscard]] bool get_bool(std::string_view name, bool fallback) const;

    /// Render the option list, for --help.
    [[nodiscard]] static std::string usage();

private:
    std::vector<std::pair<std::string, std::string>> values_;
};

/// Everything needed to reproduce one synthetic scenario across processes.
struct ScenarioSpec {
    PopulationSpec population{};
    PolicyId policy_id{};
    Generation policy_generation{};
    u64 policy_validity_ticks{0};

    [[nodiscard]] Status validate() const;
};

[[nodiscard]] ScenarioSpec scenario_from_args(const Args& args);
[[nodiscard]] PopulationSpec population_from_args(const Args& args);

/// Build the resource capacity snapshots for a scenario.
[[nodiscard]] std::vector<ResourceCapacity> build_capacities(const ScenarioSpec& spec, Tick at,
                                                            const BootIdentity& boot);

/// Build the path descriptors for a scenario.
[[nodiscard]] std::vector<PathDescriptor> build_paths(const ScenarioSpec& spec, Tick at,
                                                      const BootIdentity& boot);

/// Deterministic synthetic evidence stream for one worker shard.
struct SampleStream {
    std::vector<FlowSample> samples{};
    u64 first_flow{0};
    u64 flow_count{0};
};

/// Generate the evidence for one shard of the population. Shards partition the
/// flow identifier space so that two workers never publish the same flow.
[[nodiscard]] StatusOr<SampleStream> build_stream(const ScenarioSpec& spec, const BootIdentity& boot,
                                                  PublisherId publisher, u64 shard, u64 shards);

/// Write a text file, replacing any existing content.
[[nodiscard]] Status write_text_file(const std::string& path, const std::string& text);

/// Read a whole text file with a bounded ceiling.
[[nodiscard]] StatusOr<std::string> read_text_file(const std::string& path, std::size_t ceiling);

}  // namespace efg::cli

#endif  // EFG_APPS_CLI_HPP
