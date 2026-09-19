// Elephant Flow Governor - synthetic population benchmark.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The benchmark measures completed classification work - flows that were
// submitted, decided and (where applicable) governed - not enqueue or submission
// latency. Every population produced here is SYNTHETIC. These numbers describe
// this library on this machine and are never presented as physical network
// measurements.

#ifndef EFG_BENCHMARK_HPP
#define EFG_BENCHMARK_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "efg/checked.hpp"
#include "efg/governor.hpp"
#include "efg/status.hpp"

namespace efg {

/// Deterministic pseudo random source used only by the synthetic generator and
/// by seeded property tests. SplitMix64: tiny, fast, fully portable.
class SeededRandom {
public:
    explicit SeededRandom(u64 seed) noexcept : state_(seed) {}

    [[nodiscard]] u64 next_u64() noexcept {
        state_ += 0x9E3779B97F4A7C15ull;
        u64 z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    /// Uniform in [0, bound). bound must be positive.
    [[nodiscard]] u64 next_below(u64 bound) noexcept {
        return bound == 0 ? 0 : next_u64() % bound;
    }

    [[nodiscard]] u64 next_in(u64 lo, u64 hi) noexcept {
        if (hi <= lo) {
            return lo;
        }
        return lo + next_below(hi - lo + 1);
    }

private:
    u64 state_;
};

enum class PopulationClass : std::uint8_t {
    Mouse = 0,
    Elephant = 1,
};

/// Description of a synthetic mixed population. All volume figures are in bytes
/// per logical tick.
struct PopulationSpec {
    std::size_t mice{0};
    std::size_t elephants{0};
    u64 seed{0x5EED1234ull};

    /// Number of logical ticks the population is observed for.
    u64 ticks{64};

    /// Evidence window length in ticks.
    u64 window_ticks{4};

    /// Mouse sustained rate range, bytes per tick.
    u64 mouse_rate_lo{1};
    u64 mouse_rate_hi{64};

    /// Elephant sustained rate range, bytes per tick.
    u64 elephant_rate_lo{4096};
    u64 elephant_rate_hi{16384};

    /// Distinct capacity resources, each with capacity_bytes_per_tick.
    std::size_t resources{4};
    u64 resource_capacity{65536};
    u64 resource_reserved{8192};

    /// Distinct paths, each spanning path_resources resources.
    std::size_t paths{8};
    std::size_t path_resources{2};

    /// Number of extra threshold nodes composed into the generated policy, which
    /// raises policy complexity without changing its semantics.
    std::size_t policy_complexity{1};

    /// Portion of the population flagged protected, in basis points.
    BasisPoints protected_bp{0};

    /// Portion of the population that stops reporting early, in basis points,
    /// which produces telemetry gaps and suspension.
    BasisPoints abandon_bp{0};

    /// Tick at which the final sweep runs, measured from the start of the run.
    /// Zero selects the default horizon of one tick past the last window.
    u64 sweep_after_ticks{0};

    [[nodiscard]] Status validate() const;
};

struct BenchmarkResult {
    bool synthetic{true};
    std::size_t flows{0};
    std::size_t mice{0};
    std::size_t elephants{0};
    u64 samples_submitted{0};
    u64 samples_accepted{0};
    u64 samples_duplicate{0};
    u64 samples_rejected{0};

    u64 decisions{0};
    u64 classified_elephant{0};
    u64 classified_not_elephant{0};
    u64 classified_unknown{0};
    u64 classified_suspended{0};

    u64 intents_authorized{0};
    u64 intents_suppressed{0};
    u64 intents_clamped{0};
    u64 intents_fenced{0};
    u64 intents_revoked{0};

    /// Classification state of every tracked flow after the final sweep.
    u64 final_elephants{0};
    u64 final_not_elephants{0};
    u64 final_unknown{0};
    u64 final_suspended{0};

    u64 ticks{0};
    u64 elapsed_micros{0};
    u64 sub_second_numerator{0};

    /// Completed work per second. Derived from a monotonic clock reading of the
    /// full run, never from submission latency.
    double classifications_per_second{0.0};
    double samples_per_second{0.0};

    u64 decision_digest{0};
    u64 second_run_digest{0};
    bool deterministic_replay{false};

    GovernorCounters counters{};
};

[[nodiscard]] StatusOr<BenchmarkResult> run_benchmark(const PopulationSpec& spec,
                                                      GovernorConfig governor_config = {});

/// Build the synthetic policy document used by the benchmark.
[[nodiscard]] StatusOr<PolicyDocument> make_benchmark_policy(const PopulationSpec& spec,
                                                            PolicyId id,
                                                            Generation generation,
                                                            Tick issued_at,
                                                            u64 validity_ticks);

/// Deterministic JSON rendering of a benchmark result. The rendering marks the
/// population as synthetic in a dedicated field.
[[nodiscard]] std::string render_benchmark_json(const BenchmarkResult& result);
[[nodiscard]] std::string render_benchmark_text(const BenchmarkResult& result);

}  // namespace efg

#endif  // EFG_BENCHMARK_HPP
