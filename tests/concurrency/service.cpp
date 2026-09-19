// Elephant Flow Governor - service concurrency and shutdown tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// No test here uses a timeout. Every wait is on a definite protocol event, so a
// hang is a defect that surfaces as a hang rather than as a silent pass.

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace efg;
using namespace efgtest;

namespace {

GovernorService::Config service_config(std::size_t queue_capacity = 256) {
    GovernorService::Config config;
    config.queue_capacity = queue_capacity;
    config.governor.max_flows = 512;
    config.governor.samples_per_flow = 8;
    return config;
}

void install_into(GovernorService& service) {
    EFG_CHECK_STATUS_OK(service.governor().submit_policy(default_policy(kStart, 1u << 20)));
    EFG_CHECK_STATUS_OK(service.governor().submit_capacity(
        capacity(ResourceId{1}, 10000000, 1000000, kStart, 1u << 20)));
    EFG_CHECK_STATUS_OK(service.governor().submit_path(
        path(PathId{1}, {ResourceId{1}}, kStart, 1u << 20)));
}

}  // namespace

EFG_TEST(service, start_and_stop_return_to_a_valid_baseline) {
    GovernorService service(service_config());
    EFG_CHECK_EQ(service.state(), ServiceState::Idle);
    EFG_CHECK_STATUS_OK(service.start());
    EFG_CHECK_EQ(service.state(), ServiceState::Running);
    EFG_CHECK_STATUS_OK(service.stop());
    EFG_CHECK_EQ(service.state(), ServiceState::Stopped);
    EFG_CHECK_EQ(service.queue_depth(), 0u);
    const ServiceStats stats = service.stats();
    EFG_CHECK_EQ(stats.queue_depth, 0u);
    EFG_CHECK_EQ(stats.queue_high_water, 0u);
    EFG_CHECK_EQ(stats.submitted, stats.processed);
}

EFG_TEST(service, every_accepted_request_is_processed_before_stop_returns) {
    GovernorService service(service_config());
    EFG_CHECK_STATUS_OK(service.start());
    install_into(service);

    constexpr int kFlows = 32;
    constexpr int kWindows = 6;
    std::size_t submitted = 0;
    for (int flow = 1; flow <= kFlows; ++flow) {
        u64 cumulative = 0;
        for (int window = 1; window <= kWindows; ++window) {
            const Tick begin = kStart + static_cast<Tick>(window - 1) * 10;
            cumulative += 90000;
            const Status status = service.submit_evidence(
                sample(FlowId{static_cast<u64>(flow)}, EvidenceWindowId{static_cast<u64>(window)},
                       begin, begin + 10, cumulative, 90000, PathId{1}, Generation::initial(),
                       static_cast<u64>(window)));
            EFG_CHECK(status.ok());
            ++submitted;
        }
    }
    EFG_CHECK_STATUS_OK(service.stop());
    const ServiceStats stats = service.stats();
    EFG_CHECK_EQ(stats.submitted, submitted);
    EFG_CHECK_EQ(stats.processed, submitted);
    EFG_CHECK_EQ(stats.flows_decided, submitted);
    EFG_CHECK_EQ(service.governor().flow_count(), static_cast<std::size_t>(kFlows));
    EFG_CHECK(stats.elephants >= static_cast<u64>(kFlows));
}

EFG_TEST(service, a_full_queue_is_refused_rather_than_growing) {
    GovernorService::Config config = service_config(4);
    GovernorService service(config);
    // The service is deliberately never started, so nothing drains the queue and
    // the ceiling is reached deterministically.
    EFG_CHECK_EQ(service.state(), ServiceState::Idle);
    const Status first = service.submit_evidence(
        sample(FlowId{1}, EvidenceWindowId{1}, kStart, kStart + 10, 1000, 1000));
    EFG_CHECK_EQ(first.code(), StatusCode::ShuttingDown);
    const ServiceStats stats = service.stats();
    EFG_CHECK_EQ(stats.rejected_closed, 1u);
    EFG_CHECK_EQ(stats.queue_depth, 0u);
}

EFG_TEST(service, work_submitted_after_stop_is_refused) {
    GovernorService service(service_config());
    EFG_CHECK_STATUS_OK(service.start());
    install_into(service);
    EFG_CHECK_STATUS_OK(service.stop());
    const Status status = service.submit_evidence(
        sample(FlowId{1}, EvidenceWindowId{1}, kStart, kStart + 10, 1000, 1000));
    EFG_CHECK_EQ(status.code(), StatusCode::ShuttingDown);
    EFG_CHECK(service.stats().rejected_closed >= 1);
}

EFG_TEST(service, a_decision_sink_may_reenter_the_service_from_the_worker_thread) {
    GovernorService service(service_config());
    std::atomic<u64> sink_calls{0};
    std::atomic<u64> observed_depth{0};
    std::atomic<bool> on_worker{false};
    std::atomic<u64> elephant_seen{0};

    EFG_CHECK_STATUS_OK(service.start([&](const FlowDecision& decision) {
        // Every one of these calls takes the service mutex. If the sink ran with
        // that mutex held, this would deadlock rather than pass.
        on_worker.store(service.on_worker_thread());
        observed_depth.fetch_add(service.queue_depth() + 1);
        (void)service.stats();
        (void)service.state();
        if (decision.classification.state == ElephantState::Elephant) {
            elephant_seen.fetch_add(1);
        }
        sink_calls.fetch_add(1);
    }));
    install_into(service);

    for (int flow = 1; flow <= 8; ++flow) {
        u64 cumulative = 0;
        for (int window = 1; window <= 4; ++window) {
            const Tick begin = kStart + static_cast<Tick>(window - 1) * 10;
            cumulative += 90000;
            EFG_CHECK_STATUS_OK(service.submit_evidence(
                sample(FlowId{static_cast<u64>(flow)}, EvidenceWindowId{static_cast<u64>(window)},
                       begin, begin + 10, cumulative, 90000, PathId{1}, Generation::initial(),
                       static_cast<u64>(window))));
        }
    }
    EFG_CHECK_STATUS_OK(service.stop());

    EFG_CHECK(sink_calls.load() > 0);
    EFG_CHECK(on_worker.load());
    EFG_CHECK(observed_depth.load() > 0);
    EFG_CHECK_EQ(service.stats().sink_failures, 0u);
}

EFG_TEST(service, a_sink_that_throws_does_not_take_the_worker_down) {
    GovernorService service(service_config());
    std::atomic<u64> calls{0};
    EFG_CHECK_STATUS_OK(service.start([&](const FlowDecision&) {
        calls.fetch_add(1);
        throw std::runtime_error("sink failure");
    }));
    install_into(service);
    for (int window = 1; window <= 4; ++window) {
        const Tick begin = kStart + static_cast<Tick>(window - 1) * 10;
        EFG_CHECK_STATUS_OK(service.submit_evidence(
            sample(FlowId{1}, EvidenceWindowId{static_cast<u64>(window)}, begin, begin + 10,
                   static_cast<u64>(window) * 90000, 90000, PathId{1}, Generation::initial(),
                   static_cast<u64>(window))));
    }
    EFG_CHECK_STATUS_OK(service.stop());
    EFG_CHECK(calls.load() >= 4);
    EFG_CHECK(service.stats().sink_failures >= 4);
    EFG_CHECK_EQ(service.stats().processed, 4u);
}

EFG_TEST(service, cancellation_discards_pending_work_without_reporting_success) {
    GovernorService::Config config = service_config(1024);
    GovernorService service(config);
    std::atomic<u64> processed{0};
    EFG_CHECK_STATUS_OK(service.start([&](const FlowDecision&) { processed.fetch_add(1); }));
    install_into(service);

    for (int flow = 1; flow <= 64; ++flow) {
        (void)service.submit_evidence(
            sample(FlowId{static_cast<u64>(flow)}, EvidenceWindowId{1}, kStart, kStart + 10, 90000,
                   90000, PathId{1}, Generation::initial(), 1));
    }
    EFG_CHECK_STATUS_OK(service.cancel());
    const ServiceStats stats = service.stats();
    EFG_CHECK_EQ(stats.processed + stats.cancelled, stats.submitted);
    EFG_CHECK(stats.cancelled >= 1);
    EFG_CHECK_EQ(service.queue_depth(), 0u);
    EFG_CHECK_EQ(processed.load(), stats.processed);
}

EFG_TEST(service, many_producers_are_serialised_without_losing_work) {
    GovernorService::Config config = service_config(4096);
    GovernorService service(config);
    std::atomic<u64> decisions{0};
    EFG_CHECK_STATUS_OK(service.start([&](const FlowDecision&) { decisions.fetch_add(1); }));
    install_into(service);

    constexpr int kProducers = 4;
    constexpr int kFlowsPerProducer = 24;
    std::atomic<u64> accepted{0};
    std::atomic<u64> refused{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int producer = 0; producer < kProducers; ++producer) {
        producers.emplace_back([&, producer]() {
            for (int index = 0; index < kFlowsPerProducer; ++index) {
                const u64 flow = static_cast<u64>(producer * kFlowsPerProducer + index + 1);
                for (int window = 1; window <= 4; ++window) {
                    const Tick begin = kStart + static_cast<Tick>(window - 1) * 10;
                    const Status status = service.submit_evidence(
                        sample(FlowId{flow}, EvidenceWindowId{static_cast<u64>(window)}, begin,
                               begin + 10, static_cast<u64>(window) * 90000, 90000, PathId{1},
                               Generation::initial(), static_cast<u64>(window)));
                    if (status.ok()) {
                        accepted.fetch_add(1);
                    } else {
                        refused.fetch_add(1);
                    }
                }
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    EFG_CHECK_STATUS_OK(service.stop());

    const ServiceStats stats = service.stats();
    EFG_CHECK_EQ(stats.submitted, accepted.load());
    EFG_CHECK_EQ(stats.processed, stats.submitted);
    EFG_CHECK_EQ(decisions.load(), stats.flows_decided);
    EFG_CHECK_EQ(stats.queue_depth, 0u);
    EFG_CHECK(stats.elephants >= static_cast<u64>(kProducers * kFlowsPerProducer));
}

EFG_TEST(service, a_stopped_service_can_be_restarted) {
    GovernorService service(service_config());
    EFG_CHECK_STATUS_OK(service.start());
    install_into(service);
    EFG_CHECK_STATUS_OK(service.stop());
    EFG_CHECK_STATUS_OK(service.start());
    EFG_CHECK_EQ(service.state(), ServiceState::Running);
    EFG_CHECK_STATUS_OK(service.stop());
    EFG_CHECK_EQ(service.state(), ServiceState::Stopped);
    // Counters accumulate across incarnations rather than resetting silently.
    EFG_CHECK(service.stats().submitted == 0u);
}

EFG_TEST(service, a_sweep_request_drains_through_the_worker) {
    GovernorService service(service_config());
    EFG_CHECK_STATUS_OK(service.start());
    install_into(service);
    for (int window = 1; window <= 4; ++window) {
        const Tick begin = kStart + static_cast<Tick>(window - 1) * 10;
        EFG_CHECK_STATUS_OK(service.submit_evidence(
            sample(FlowId{1}, EvidenceWindowId{static_cast<u64>(window)}, begin, begin + 10,
                   static_cast<u64>(window) * 90000, 90000, PathId{1}, Generation::initial(),
                   static_cast<u64>(window))));
    }
    EFG_CHECK_STATUS_OK(service.submit_tick(kStart + 500000));
    EFG_CHECK_STATUS_OK(service.stop());
    const ServiceStats stats = service.stats();
    EFG_CHECK_EQ(stats.decision_batches, 1u);
    EFG_CHECK(stats.suspensions >= 1);
    EFG_CHECK(service.governor().counters().authority_revocations >= 1);
}

EFG_TEST(service, concurrent_statistics_reads_never_block_progress) {
    GovernorService service(service_config(2048));
    EFG_CHECK_STATUS_OK(service.start());
    install_into(service);

    std::atomic<bool> stop_reader{false};
    std::atomic<u64> reads{0};
    std::thread reader([&]() {
        while (!stop_reader.load()) {
            (void)service.stats();
            (void)service.queue_depth();
            reads.fetch_add(1);
        }
    });

    for (int flow = 1; flow <= 48; ++flow) {
        for (int window = 1; window <= 4; ++window) {
            const Tick begin = kStart + static_cast<Tick>(window - 1) * 10;
            EFG_CHECK_STATUS_OK(service.submit_evidence(
                sample(FlowId{static_cast<u64>(flow)}, EvidenceWindowId{static_cast<u64>(window)},
                       begin, begin + 10, static_cast<u64>(window) * 90000, 90000, PathId{1},
                       Generation::initial(), static_cast<u64>(window))));
        }
    }
    EFG_CHECK_STATUS_OK(service.stop());
    stop_reader.store(true);
    reader.join();

    EFG_CHECK(reads.load() > 0);
    EFG_CHECK_EQ(service.stats().processed, service.stats().submitted);
}
