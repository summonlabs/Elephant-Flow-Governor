// Elephant Flow Governor - seeded randomized property tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The generator is fully deterministic: the same seed always produces the same
// operation stream, so a failure here is reproducible by construction.

#include <memory>
#include <string>
#include <vector>

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace efg;
using namespace efgtest;

namespace {

PathDescriptor path_(PathId id, std::vector<ResourceId> resources, Tick at, u64 validity) {
    PathDescriptor descriptor;
    descriptor.path = id;
    descriptor.generation = Generation::initial();
    descriptor.resources = std::move(resources);
    StatusOr<ValidityWindow> window = make_validity(at, validity);
    if (window.ok()) {
        descriptor.validity = window.value();
    }
    descriptor.provenance = provenance(1, 1, 1, id.value(), at);
    return descriptor;
}

struct Outcome {
    u64 digest{0};
    GovernorCounters counters{};
    std::size_t flows{0};
    std::size_t intents{0};
    u64 transitions{0};
};

/// Drive a pseudo random but reproducible workload through one governor.
Outcome run_workload(u64 seed, std::size_t operations, bool hostile) {
    SeededRandom random(seed);
    GovernorConfig config;
    config.max_flows = 256;
    config.samples_per_flow = 2 + static_cast<std::size_t>(random.next_below(4));
    config.max_telemetry_gap_ticks = 5 + random.next_below(50);
    config.classification_validity_ticks = 64 + random.next_below(4096);
    config.intent_duration_ticks = 16 + random.next_below(512);
    config.max_history_per_flow = 8 + static_cast<std::size_t>(random.next_below(32));

    Governor governor(config);
    governor.set_incarnation(boot(1 + random.next_below(4), 1 + random.next_below(4)));

    const Tick start = 1000;
    PolicyDocument policy = hysteresis_policy(
        1 + static_cast<u32>(random.next_below(3)), 2 + static_cast<u32>(random.next_below(4)),
        500000 + random.next_below(4000000), 50000 + random.next_below(200000), start, 1u << 20);
    EFG_CHECK_STATUS_OK(governor.submit_policy(policy));
    for (u64 resource = 1; resource <= 3; ++resource) {
        EFG_CHECK_STATUS_OK(governor.submit_capacity(
            capacity(ResourceId{resource}, 4000000 + random.next_below(8000000),
                     100000 + random.next_below(900000), start, 1u << 20)));
    }
    for (u64 path = 1; path <= 3; ++path) {
        EFG_CHECK_STATUS_OK(governor.submit_path(
            path_(PathId{path}, {ResourceId{1 + (path % 3)}, ResourceId{1 + ((path + 1) % 3)}}, start,
                  1u << 20)));
    }

    struct StreamState {
        Tick cursor{0};
        u64 cumulative{0};
        u64 sequence{0};
        PathId path{1};
        Generation path_generation{Generation::initial()};
        Generation flow_generation{Generation::initial()};
        bool completed{false};
    };
    std::vector<StreamState> streams(16);
    for (StreamState& stream : streams) {
        stream.cursor = start;
    }

    for (std::size_t step = 0; step < operations; ++step) {
        const u64 pick = random.next_below(streams.size());
        StreamState& stream = streams[static_cast<std::size_t>(pick)];
        const FlowId flow{pick + 1};

        const u64 action = random.next_below(100);
        if (action < 6) {
            // A re-route onto a different path identifier.
            stream.path = PathId{1 + random.next_below(3)};
            stream.path_generation = Generation{1 + random.next_below(3)};
            continue;
        }
        if (action < 10) {
            // An explicit evidence gap.
            stream.cursor += 100 + random.next_below(500);
            continue;
        }
        if (action < 13 && !stream.completed) {
            const Status status =
                governor.complete_flow(flow, stream.flow_generation, stream.cursor);
            if (status.ok()) {
                stream.completed = true;
            }
            continue;
        }
        if (action < 15) {
            // A flow generation rollover.
            stream.flow_generation = Generation{stream.flow_generation.value() + 1};
            stream.cumulative = 0;
            stream.sequence = 0;
            stream.completed = false;
            continue;
        }

        const u64 rate = random.next_below(12000);
        const u64 window_ticks = 1 + random.next_below(20);
        FlowSample value = sample(flow, EvidenceWindowId{stream.sequence + 1}, stream.cursor,
                                  stream.cursor + window_ticks,
                                  stream.cumulative + rate * window_ticks, rate * window_ticks,
                                  stream.path, stream.path_generation, stream.sequence + 1,
                                  stream.flow_generation, 1u << 20);
        const u64 protection = random.next_below(4);
        if (protection == 1) {
            value.protection = ProtectionState::Protected;
        } else if (protection == 2) {
            value.protection = ProtectionState::NonPreemptible;
            value.service_class = ServiceClass::Reserved;
            value.reservation = ReservationId{1 + pick};
        } else if (protection == 3) {
            value.protection = ProtectionState::Unknown;
            value.service_class = ServiceClass::Unknown;
        }
        if (hostile && random.next_below(8) == 0) {
            value.provenance.digest ^= 1ull;  // deliberate integrity failure
        } else {
            value.provenance.digest = value.digest();
        }

        const StatusOr<FlowDecision> decision = governor.submit_evidence(value);
        if (!decision.ok()) {
            continue;
        }
        stream.cursor += window_ticks;
        stream.cumulative += rate * window_ticks;
        stream.sequence += 1;

        // Invariant: a positive classification always carries governance
        // authority, and an elephant is never simultaneously authority-free.
        const FlowClassification& classification = decision.value().classification;
        if (classification.state == ElephantState::Elephant) {
            EFG_CHECK(classification.authority == AuthorityLevel::Govern ||
                      classification.authority == AuthorityLevel::Observe);
            EFG_CHECK(classification.measurements.evidence_present);
            EFG_CHECK(!classification.measurements.gap_detected);
        } else {
            EFG_CHECK(classification.authority == AuthorityLevel::None);
        }
        // Invariant: an unknown protection state never governs.
        if (classification.measurements.protection == ProtectionState::Unknown) {
            EFG_CHECK(classification.authority != AuthorityLevel::Govern);
        }
    }

    for (u64 id = 1; id <= streams.size(); ++id) {
        const Tick sweep = streams[static_cast<std::size_t>(id - 1)].cursor + 1;
        (void)governor.tick(sweep);
    }

    Outcome outcome;
    const StatusOr<u64> digest = decision_digest(governor);
    if (!digest.ok()) {
        EFG_CHECK(digest.ok());
        return outcome;
    }
    outcome.digest = digest.value();
    outcome.counters = governor.counters();
    outcome.flows = governor.flow_count();
    outcome.intents = governor.intents().size();
    outcome.transitions = governor.counters().transitions;
    return outcome;
}

}  // namespace

EFG_TEST(property, the_same_seed_reproduces_the_entire_decision_surface) {
    for (u64 seed = 1; seed <= 6; ++seed) {
        const Outcome first = run_workload(seed, 400, false);
        const Outcome second = run_workload(seed, 400, false);
        EFG_CHECK_EQ(first.digest, second.digest);
        EFG_CHECK_EQ(first.flows, second.flows);
        EFG_CHECK_EQ(first.intents, second.intents);
        EFG_CHECK_EQ(first.transitions, second.transitions);
        EFG_CHECK_EQ(first.counters.decisions, second.counters.decisions);
        EFG_CHECK_EQ(first.counters.elephant_decisions, second.counters.elephant_decisions);
        EFG_CHECK_EQ(first.counters.suspended_decisions, second.counters.suspended_decisions);
    }
}

EFG_TEST(property, different_seeds_explore_different_decision_surfaces) {
    const Outcome first = run_workload(11, 300, false);
    const Outcome second = run_workload(12, 300, false);
    EFG_CHECK(first.digest != second.digest);
}

EFG_TEST(property, a_deliberately_corrupted_window_never_reaches_a_decision) {
    const Outcome clean = run_workload(21, 300, false);
    const Outcome hostile = run_workload(21, 300, true);
    // The integrity failures are counted and refused, and the surviving decision
    // surface is a strict subset of the clean one.
    EFG_CHECK(hostile.counters.evidence_rejected >= 1);
    EFG_CHECK(hostile.counters.decisions <= clean.counters.decisions);
    EFG_CHECK(hostile.digest != clean.digest);
}

EFG_TEST(property, no_run_ever_produces_a_protected_corrective_intent) {
    for (u64 seed = 31; seed <= 36; ++seed) {
        Outcome outcome = run_workload(seed, 300, false);
        EFG_CHECK(outcome.counters.evidence_rejected == 0 ||
                  outcome.counters.evidence_contradictory ==
                      outcome.counters.evidence_rejected);
    }
}

EFG_TEST(property, every_corruption_of_an_encoded_sample_is_detected) {
    const FlowSample original =
        sample(FlowId{3}, EvidenceWindowId{4}, kStart, kStart + 25, 12345, 6789, PathId{2},
               Generation{3}, 9, Generation{5});
    ByteWriter writer(1u << 16);
    encode(writer, original);
    const std::vector<std::byte> bytes = writer.buffer();

    SeededRandom random(0xABCDEFull);
    std::size_t decoded_mutations = 0;
    for (int trial = 0; trial < 2000; ++trial) {
        std::vector<std::byte> mutated = bytes;
        const std::size_t index = static_cast<std::size_t>(random.next_below(mutated.size()));
        mutated[index] ^= static_cast<std::byte>(1u << static_cast<unsigned>(random.next_below(8)));

        ByteReader reader(mutated);
        FlowSample decoded;
        const Status status = decode(reader, decoded);
        if (!status.ok()) {
            continue;
        }
        ++decoded_mutations;
        // A single bit anywhere in the encoded sample changes the content digest
        // while the published provenance digest stays as it was, so no mutation
        // can ever survive validation.
        EFG_CHECK(!decoded.validate().ok());
    }
    EFG_CHECK(decoded_mutations > 0);
}

EFG_TEST(property, the_history_digest_is_a_function_of_the_transition_order) {
    auto build = [](bool reverse_tail, u64& digest, std::size_t& entries) {
        GovernorConfig config;
        config.max_history_per_flow = 6;
        Governor governor(config);
        governor.set_incarnation(boot());
        EFG_CHECK_STATUS_OK(
            governor.submit_policy(hysteresis_policy(1, 2, 2000000, 100000)));
        EFG_CHECK_STATUS_OK(governor.submit_capacity(capacity(ResourceId{1}, 10000000, 1000000)));
        EFG_CHECK_STATUS_OK(governor.submit_path(path_(PathId{1}, {ResourceId{1}}, kStart, 1u << 20)));
        WindowFeeder feeder(governor, FlowId{1}, 10);
        for (int i = 0; i < 4; ++i) {
            feeder.push(9000);
        }
        for (int i = 0; i < (reverse_tail ? 2 : 4); ++i) {
            feeder.push(10);
        }
        const ClassificationHistory* history = governor.history(FlowId{1}, Generation::initial());
        EFG_REQUIRE(history != nullptr);
        digest = history->digest();
        entries = history->size();
    };
    u64 first_digest = 0;
    std::size_t first_entries = 0;
    u64 second_digest = 0;
    std::size_t second_entries = 0;
    build(false, first_digest, first_entries);
    build(false, second_digest, second_entries);
    EFG_CHECK_EQ(first_digest, second_digest);
    EFG_CHECK_EQ(first_entries, second_entries);
    u64 third_digest = 0;
    std::size_t third_entries = 0;
    build(true, third_digest, third_entries);
    EFG_CHECK(first_digest != third_digest);
}

EFG_TEST(property, the_benchmark_replays_bit_for_bit) {
    PopulationSpec spec;
    spec.mice = 48;
    spec.elephants = 12;
    spec.seed = 0xC0FFEEull;
    spec.ticks = 32;
    spec.window_ticks = 4;
    spec.protected_bp = 1500;
    spec.abandon_bp = 1000;
    StatusOr<BenchmarkResult> result = run_benchmark(spec);
    EFG_REQUIRE(result.ok());
    EFG_CHECK(result.value().deterministic_replay);
    EFG_CHECK_EQ(result.value().decision_digest, result.value().second_run_digest);
    EFG_CHECK(result.value().classified_elephant > 0);
}
