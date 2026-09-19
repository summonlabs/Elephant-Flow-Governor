// Elephant Flow Governor - synthetic benchmark tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace efg;
using namespace efgtest;

namespace {

PopulationSpec mixed_population() {
    PopulationSpec spec;
    spec.mice = 96;
    spec.elephants = 24;
    spec.seed = 0x1234ABCDull;
    spec.ticks = 48;
    spec.window_ticks = 4;
    spec.resources = 4;
    spec.resource_capacity = 65536;
    spec.resource_reserved = 8192;
    spec.paths = 8;
    spec.path_resources = 2;
    spec.policy_complexity = 3;
    return spec;
}

}  // namespace

EFG_TEST(benchmark, a_mixed_population_separates_mice_from_elephants) {
    StatusOr<BenchmarkResult> result = run_benchmark(mixed_population());
    EFG_REQUIRE(result.ok());
    const BenchmarkResult& value = result.value();
    EFG_CHECK(value.synthetic);
    EFG_CHECK_EQ(value.flows, 120u);
    EFG_CHECK_EQ(value.mice, 96u);
    EFG_CHECK_EQ(value.elephants, 24u);
    EFG_CHECK(value.samples_accepted > 0);
    EFG_CHECK_EQ(value.samples_rejected, 0u);
    EFG_CHECK(value.classified_elephant > 0);
    EFG_CHECK(value.classified_not_elephant > value.classified_elephant);
    EFG_CHECK(value.decisions >= value.samples_accepted);
    EFG_CHECK(value.deterministic_replay);
    EFG_CHECK_EQ(value.decision_digest, value.second_run_digest);
}

EFG_TEST(benchmark, the_rendered_result_is_labelled_synthetic) {
    StatusOr<BenchmarkResult> result = run_benchmark(mixed_population());
    EFG_REQUIRE(result.ok());
    const std::string json = render_benchmark_json(result.value());
    const std::string text = render_benchmark_text(result.value());
    EFG_CHECK(json.find("population_kind") != std::string::npos);
    EFG_CHECK(json.find("SYNTHETIC") != std::string::npos);
    EFG_CHECK(json.find("physical_network") != std::string::npos);
    EFG_CHECK(text.find("SYNTHETIC") != std::string::npos);
    EFG_CHECK(text.find("deterministic=true") != std::string::npos);
    EFG_CHECK(json.find("classifications_per_second") != std::string::npos);
}

EFG_TEST(benchmark, protected_flows_are_classified_but_never_correctively_governed) {
    PopulationSpec spec = mixed_population();
    spec.protected_bp = 3000;
    StatusOr<BenchmarkResult> result = run_benchmark(spec);
    EFG_REQUIRE(result.ok());
    EFG_CHECK(result.value().classified_elephant > 0);
    EFG_CHECK(result.value().deterministic_replay);
}

EFG_TEST(benchmark, abandonment_produces_revalidation_rather_than_a_stale_elephant) {
    PopulationSpec spec = mixed_population();
    spec.abandon_bp = 5000;
    // Sweep at a horizon that separates the two populations: the publishers that
    // stopped reporting have aged out, the ones still reporting have not.
    const u64 windows = spec.ticks / spec.window_ticks;
    const u64 abandoned_last_begin = (windows / 2u) * spec.window_ticks - spec.window_ticks;
    const u64 active_last_begin = spec.ticks - spec.window_ticks;
    const u64 evidence_validity = spec.ticks * 4u + 64u;
    spec.sweep_after_ticks =
        ((abandoned_last_begin + active_last_begin) / 2u) + evidence_validity;

    StatusOr<BenchmarkResult> result = run_benchmark(spec);
    EFG_REQUIRE(result.ok());
    EFG_CHECK(result.value().final_suspended > 0);
    EFG_CHECK(result.value().final_elephants > 0);
    EFG_CHECK(result.value().final_suspended < result.value().final_elephants);
    EFG_CHECK(result.value().deterministic_replay);
}

EFG_TEST(benchmark, policy_complexity_changes_the_document_but_not_the_semantics) {
    PopulationSpec simple = mixed_population();
    simple.policy_complexity = 1;
    PopulationSpec complex = mixed_population();
    complex.policy_complexity = 12;

    StatusOr<PolicyDocument> simple_policy =
        make_benchmark_policy(simple, PolicyId{7}, Generation::initial(), kStart, 4096);
    StatusOr<PolicyDocument> complex_policy =
        make_benchmark_policy(complex, PolicyId{7}, Generation::initial(), kStart, 4096);
    EFG_REQUIRE(simple_policy.ok());
    EFG_REQUIRE(complex_policy.ok());
    EFG_CHECK(complex_policy.value().enter_rule.nodes.size() >
              simple_policy.value().enter_rule.nodes.size());
    EFG_CHECK(simple_policy.value().digest != complex_policy.value().digest);

    StatusOr<BenchmarkResult> simple_run = run_benchmark(simple);
    StatusOr<BenchmarkResult> complex_run = run_benchmark(complex);
    EFG_REQUIRE(simple_run.ok());
    EFG_REQUIRE(complex_run.ok());
    // The extra Always leaves cannot change which flows qualify.
    EFG_CHECK_EQ(simple_run.value().classified_elephant, complex_run.value().classified_elephant);
    EFG_CHECK_EQ(simple_run.value().classified_not_elephant,
                 complex_run.value().classified_not_elephant);
}

EFG_TEST(benchmark, hostile_specifications_are_refused_before_any_work) {
    PopulationSpec empty;
    EFG_CHECK_EQ(empty.validate().code(), StatusCode::InvalidArgument);

    PopulationSpec overlapping = mixed_population();
    overlapping.elephant_rate_lo = overlapping.mouse_rate_hi;
    EFG_CHECK_EQ(overlapping.validate().code(), StatusCode::InvalidArgument);

    PopulationSpec oversized = mixed_population();
    oversized.mice = limits::kMaxBenchmarkPopulation + 1;
    EFG_CHECK_EQ(oversized.validate().code(), StatusCode::Oversized);

    PopulationSpec inverted = mixed_population();
    inverted.mouse_rate_hi = inverted.mouse_rate_lo - 1;
    EFG_CHECK_EQ(inverted.validate().code(), StatusCode::InvalidArgument);

    PopulationSpec no_headroom = mixed_population();
    no_headroom.resource_reserved = no_headroom.resource_capacity;
    EFG_CHECK_EQ(no_headroom.validate().code(), StatusCode::InvalidArgument);

    PopulationSpec long_window = mixed_population();
    long_window.window_ticks = long_window.ticks + 1;
    EFG_CHECK_EQ(long_window.validate().code(), StatusCode::InvalidArgument);
}

EFG_TEST(benchmark, the_measured_work_is_classification_not_submission) {
    PopulationSpec spec = mixed_population();
    spec.mice = 8;
    spec.elephants = 2;
    spec.ticks = 16;
    StatusOr<BenchmarkResult> result = run_benchmark(spec);
    EFG_REQUIRE(result.ok());
    EFG_CHECK(result.value().decisions > 0);
    EFG_CHECK(result.value().elapsed_micros > 0);
    EFG_CHECK(result.value().classifications_per_second > 0.0);
    EFG_CHECK(result.value().samples_per_second > 0.0);
}
