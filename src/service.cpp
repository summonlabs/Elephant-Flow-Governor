// Elephant Flow Governor - single process service wrapper.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Lock discipline is documented in the header and enforced structurally here:
// the single mutex guards the queue, the state machine and the queue accounting;
// it is released before any governor call and before any user callable runs; and
// it is always released before joining the worker.

#include "efg/service.hpp"

#include <utility>

namespace efg {

std::string_view to_string(ServiceState state) noexcept {
    switch (state) {
        case ServiceState::Idle: return "idle";
        case ServiceState::Running: return "running";
        case ServiceState::Stopping: return "stopping";
        case ServiceState::Stopped: return "stopped";
    }
    return "idle";
}

GovernorService::GovernorService(Config config) : config_(config), governor_(config.governor) {
    if (config_.queue_capacity == 0) {
        config_.queue_capacity = 1;
    }
    if (config_.queue_capacity > limits::kMaxQueueCapacity) {
        config_.queue_capacity = limits::kMaxQueueCapacity;
    }
}

GovernorService::~GovernorService() {
    // Destruction must never join while holding the lock, and must never leave a
    // joinable thread behind. stop() is idempotent.
    (void)stop();
}

Status GovernorService::start(DecisionSink sink) {
    {
        std::lock_guard<std::mutex> guard(mu_);
        if (state_ == ServiceState::Running || state_ == ServiceState::Stopping) {
            return make_status(StatusCode::AlreadyExists, "service is already running");
        }
        if (worker_.joinable()) {
            return make_status(StatusCode::Busy, "previous worker thread has not been joined");
        }
        queue_.clear();
        queue_high_water_ = 0;
        accepting_ = true;
        drain_ = true;
        state_ = ServiceState::Running;
    }
    // The thread is created after the state transition and after the lock is
    // released, so there is no window in which the worker observes a partial
    // service state or blocks on a mutex its creator still holds.
    worker_ = std::thread([this, sink = std::move(sink)]() mutable {
        worker_main(std::move(sink));
    });
    return {};
}

Status GovernorService::submit(ServiceRequest request) {
    {
        std::lock_guard<std::mutex> guard(mu_);
        if (!accepting_ || state_ != ServiceState::Running) {
            ++rejected_closed_;
            return make_status(StatusCode::ShuttingDown, "service is not accepting work");
        }
        if (queue_.size() >= config_.queue_capacity) {
            ++rejected_full_;
            return make_status(StatusCode::CapacityExceeded, "service queue is full");
        }
        queue_.push_back(std::move(request));
        if (queue_.size() > queue_high_water_) {
            queue_high_water_ = queue_.size();
        }
        ++submitted_;
    }
    cv_not_empty_.notify_one();
    return {};
}

Status GovernorService::submit_evidence(const FlowSample& sample) {
    ServiceRequest request;
    request.kind = RequestKind::Evidence;
    request.sample = sample;
    request.flow = sample.flow;
    request.generation = sample.flow_generation;
    request.tick = sample.span.end;
    return submit(std::move(request));
}

Status GovernorService::submit_completion(FlowId flow, Generation generation, Tick at) {
    ServiceRequest request;
    request.kind = RequestKind::Completion;
    request.flow = flow;
    request.generation = generation;
    request.tick = at;
    return submit(std::move(request));
}

Status GovernorService::submit_tick(Tick now) {
    ServiceRequest request;
    request.kind = RequestKind::Tick;
    request.tick = now;
    return submit(std::move(request));
}

void GovernorService::worker_main(DecisionSink sink) {
    {
        std::lock_guard<std::mutex> guard(mu_);
        worker_id_ = std::this_thread::get_id();
    }

    while (true) {
        ServiceRequest request;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_not_empty_.wait(lock, [this]() {
                return !queue_.empty() || state_ != ServiceState::Running;
            });
            if (queue_.empty()) {
                if (state_ != ServiceState::Running) {
                    break;
                }
                continue;
            }
            request = std::move(queue_.front());
            queue_.pop_front();
        }
        cv_not_full_.notify_all();

        // The lock is released for the whole of the processing step. The
        // governor is therefore touched by exactly one thread, and a decision
        // sink is free to call back into stats() or queue_depth().
        process_request(request, sink);

        std::lock_guard<std::mutex> guard(mu_);
        if (queue_.empty()) {
            cv_idle_.notify_all();
        }
    }

    std::lock_guard<std::mutex> guard(mu_);
    state_ = ServiceState::Stopped;
    cv_idle_.notify_all();
    cv_not_full_.notify_all();
}

void GovernorService::process_request(ServiceRequest& request, const DecisionSink& sink) {
    switch (request.kind) {
        case RequestKind::Evidence: {
            StatusOr<FlowDecision> decision = governor_.submit_evidence(request.sample);
            if (decision.ok()) {
                ++flows_decided_;
                if (decision.value().classification.state == ElephantState::Elephant) {
                    ++elephants_;
                }
                if (decision.value().classification.state == ElephantState::Suspended) {
                    ++suspensions_;
                }
                if (sink) {
                    invoke_sink(sink, decision.value());
                }
            }
            break;
        }
        case RequestKind::Completion: {
            (void)governor_.complete_flow(request.flow, request.generation, request.tick);
            break;
        }
        case RequestKind::Tick: {
            StatusOr<DecisionBatch> batch = governor_.tick(request.tick);
            if (batch.ok()) {
                ++decision_batches_;
                for (const FlowDecision& decision : batch.value().decisions) {
                    ++flows_decided_;
                    if (decision.classification.state == ElephantState::Suspended) {
                        ++suspensions_;
                    }
                    if (sink) {
                        invoke_sink(sink, decision);
                    }
                }
            }
            break;
        }
    }
    ++processed_;
    const u64 previous = last_tick_.load();
    if (request.tick > previous) {
        last_tick_.store(request.tick);
    }
}

void GovernorService::invoke_sink(const DecisionSink& sink, const FlowDecision& decision) {
    ++sinks_invoked_;
    try {
        sink(decision);
    } catch (...) {
        // A failing sink must never take the worker down with it.
        ++sink_failures_;
    }
}

Status GovernorService::stop() {
    bool already_stopped = false;
    {
        std::lock_guard<std::mutex> guard(mu_);
        accepting_ = false;
        drain_ = true;
        if (state_ == ServiceState::Running) {
            state_ = ServiceState::Stopping;
        }
        if (state_ == ServiceState::Idle) {
            already_stopped = true;
        }
        cv_not_empty_.notify_all();
    }
    cv_not_empty_.notify_all();

    // Joining happens with no lock held, so the worker can always reach the
    // predicate it is waiting on and complete its shutdown handshake.
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> guard(mu_);
        if (!already_stopped || state_ == ServiceState::Stopping) {
            state_ = ServiceState::Stopped;
        }
        queue_.clear();
        queue_high_water_ = 0;
        accepting_ = false;
    }
    return {};
}

Status GovernorService::cancel() {
    {
        std::lock_guard<std::mutex> guard(mu_);
        accepting_ = false;
        drain_ = false;
        if (state_ == ServiceState::Running) {
            state_ = ServiceState::Stopping;
        }
        cancelled_ += static_cast<u64>(queue_.size());
        queue_.clear();
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> guard(mu_);
        state_ = ServiceState::Stopped;
        queue_.clear();
        queue_high_water_ = 0;
    }
    return {};
}

ServiceState GovernorService::state() const noexcept {
    std::lock_guard<std::mutex> guard(mu_);
    return state_;
}

std::size_t GovernorService::queue_depth() const {
    std::lock_guard<std::mutex> guard(mu_);
    return queue_.size();
}

ServiceStats GovernorService::stats() const {
    ServiceStats out;
    {
        std::lock_guard<std::mutex> guard(mu_);
        out.queue_depth = queue_.size();
        out.queue_high_water = queue_high_water_;
    }
    out.submitted = submitted_.load();
    out.processed = processed_.load();
    out.rejected_full = rejected_full_.load();
    out.rejected_closed = rejected_closed_.load();
    out.cancelled = cancelled_.load();
    out.decision_batches = decision_batches_.load();
    out.flows_decided = flows_decided_.load();
    out.elephants = elephants_.load();
    out.suspensions = suspensions_.load();
    out.sinks_invoked = sinks_invoked_.load();
    out.sink_failures = sink_failures_.load();
    out.last_tick = last_tick_.load();
    return out;
}

}  // namespace efg
