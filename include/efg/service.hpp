// Elephant Flow Governor - single process service wrapper.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Lock discipline, stated once and enforced structurally:
//
//  * There is exactly one mutex, mu_, and it guards the request queue, the
//    service state machine and the counters. It guards nothing else.
//  * The worker thread never holds mu_ while touching the Governor. The Governor
//    is therefore single threaded by construction and needs no lock of its own.
//  * No user supplied callable is ever invoked while mu_ is held. The decision
//    sink runs on the worker thread with the lock released, which makes
//    re-entrant queries from a sink legal rather than deadlocking.
//  * Condition variable waits always use the predicate form, so a spurious or
//    lost wakeup cannot produce an unbounded wait.
//  * stop() and cancel() release mu_ before joining, so the worker can always
//    make progress and complete the handshake it needs to exit.
//  * There is no nested lock acquisition anywhere: mu_ is a leaf lock.

#ifndef EFG_SERVICE_HPP
#define EFG_SERVICE_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "efg/governor.hpp"
#include "efg/limits.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"

namespace efg {

enum class ServiceState : std::uint8_t {
    Idle = 0,
    Running = 1,
    Stopping = 2,
    Stopped = 3,
};

[[nodiscard]] std::string_view to_string(ServiceState state) noexcept;

enum class RequestKind : std::uint8_t {
    Evidence = 0,
    Tick = 1,
    Completion = 2,
};

struct ServiceRequest {
    RequestKind kind{RequestKind::Evidence};
    FlowSample sample{};
    FlowId flow{};
    Generation generation{};
    Tick tick{0};
};

struct ServiceStats {
    u64 submitted{0};
    u64 processed{0};
    u64 rejected_full{0};
    u64 rejected_closed{0};
    u64 cancelled{0};
    u64 decision_batches{0};
    u64 flows_decided{0};
    u64 elephants{0};
    u64 suspensions{0};
    u64 sinks_invoked{0};
    u64 sink_failures{0};
    std::size_t queue_depth{0};
    std::size_t queue_high_water{0};
    Tick last_tick{0};
};

/// Handles a decision produced by the worker. Invoked with no lock held.
using DecisionSink = std::function<void(const FlowDecision&)>;

class GovernorService {
public:
    struct Config {
        std::size_t queue_capacity{limits::kDefaultQueueCapacity};
        GovernorConfig governor{};
    };

    explicit GovernorService(Config config = {});
    ~GovernorService();

    GovernorService(const GovernorService&) = delete;
    GovernorService& operator=(const GovernorService&) = delete;

    /// Start the worker thread. Passing an empty sink is allowed.
    Status start(DecisionSink sink = {});

    /// Enqueue one request. Fails with CapacityExceeded when the bounded queue is
    /// full, and with ShuttingDown once the service stops accepting work.
    Status submit(ServiceRequest request);
    Status submit_evidence(const FlowSample& sample);
    Status submit_completion(FlowId flow, Generation generation, Tick at);
    Status submit_tick(Tick now);

    /// Stop accepting work, let every accepted request complete, then join.
    /// Returns once the governor's accounting is back at a valid baseline.
    Status stop();

    /// Stop accepting work, discard pending requests, then join. Requests already
    /// being processed finish; nothing after them is processed or recorded.
    Status cancel();

    [[nodiscard]] ServiceState state() const noexcept;
    [[nodiscard]] ServiceStats stats() const;
    [[nodiscard]] std::size_t queue_depth() const;

    /// Read-only access to the governor. Safe only after the worker has stopped,
    /// or from inside a decision sink, which runs on the worker thread itself.
    [[nodiscard]] Governor& governor() noexcept { return governor_; }
    [[nodiscard]] const Governor& governor() const noexcept { return governor_; }

    [[nodiscard]] bool worker_running() const noexcept { return worker_.joinable(); }
    [[nodiscard]] bool on_worker_thread() const noexcept {
        return std::this_thread::get_id() == worker_id_;
    }

private:
    void worker_main(DecisionSink sink);
    void process_request(ServiceRequest& request, const DecisionSink& sink);
    void invoke_sink(const DecisionSink& sink, const FlowDecision& decision);

    mutable std::mutex mu_{};
    std::condition_variable cv_not_empty_{};
    std::condition_variable cv_not_full_{};
    std::condition_variable cv_idle_{};

    Config config_{};
    std::deque<ServiceRequest> queue_{};
    ServiceState state_{ServiceState::Idle};
    std::size_t queue_high_water_{0};
    bool accepting_{false};
    bool drain_{true};

    Governor governor_{};
    std::thread worker_{};
    std::thread::id worker_id_{};

    // Counters touched only by the worker thread; published through atomics so
    // that stats() reads them without ever taking a lock the worker needs.
    std::atomic<u64> submitted_{0};
    std::atomic<u64> processed_{0};
    std::atomic<u64> rejected_full_{0};
    std::atomic<u64> rejected_closed_{0};
    std::atomic<u64> cancelled_{0};
    std::atomic<u64> decision_batches_{0};
    std::atomic<u64> flows_decided_{0};
    std::atomic<u64> elephants_{0};
    std::atomic<u64> suspensions_{0};
    std::atomic<u64> sinks_invoked_{0};
    std::atomic<u64> sink_failures_{0};
    std::atomic<u64> last_tick_{0};
};

}  // namespace efg

#endif  // EFG_SERVICE_HPP
