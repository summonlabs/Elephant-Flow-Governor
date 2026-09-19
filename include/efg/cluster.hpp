// Elephant Flow Governor - cluster coordinator and worker.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Real OS processes, real loopback sockets, real framing. A coordinator owns the
// authoritative governor and an epoch. A worker is a publisher: it observes flows
// and submits generation bound evidence, and it holds no authority of its own.
//
// Sessions are served one at a time, in accept order. That design choice removes
// an entire class of lock-ordering hazards: the coordinator's governor is
// single threaded, and no lock is ever held across a socket operation.
//
// Fencing rules, all fail closed:
//   * a Hello carrying an epoch below the coordinator's is fenced;
//   * a Hello carrying an epoch above the coordinator's is fenced;
//   * a worker re-registering with a different boot incarnation supersedes its
//     previous incarnation, which is fenced along with its live intent;
//   * a worker re-registering with the same boot while a session is already open
//     is a duplicate and is fenced;
//   * any frame whose epoch, boot or worker identity disagrees with the
//     registered session is fenced and the connection is closed.

#ifndef EFG_CLUSTER_HPP
#define EFG_CLUSTER_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "efg/checked.hpp"
#include "efg/governor.hpp"
#include "efg/identity.hpp"
#include "efg/limits.hpp"
#include "efg/persistence.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"
#include "efg/transport.hpp"

namespace efg {

struct ClusterLimits {
    std::size_t max_workers{limits::kMaxWorkers};
    std::size_t max_payload{limits::kMaxFramePayload};
    std::size_t max_batch{limits::kMaxEvidenceBatch};
};

struct SessionOutcome {
    WorkerId worker{};
    BootId boot{};
    EpochId epoch{};
    std::string label{};
    bool fenced{false};
    bool clean_bye{false};
    FenceReason fence{FenceReason::None};
    u64 frames{0};
    u64 samples{0};
    u64 accepted{0};
    u64 duplicates{0};
    u64 rejected{0};
    u64 intents_fenced{0};
    std::string detail{};
};

struct WorkerRecord {
    WorkerId worker{};
    BootId boot{};
    EpochId epoch{};
    std::string label{};
    u64 sessions{0};
    u64 fences{0};
    Tick last_seen{0};
    std::string outcome{};
};

struct CoordinatorConfig {
    BootIdentity boot{};
    ClusterLimits limits{};
    GovernorConfig governor{};
    Tick start_tick{0};
    u64 tick_stride{1};
    /// Number of sessions to serve before run() returns. 0 means serve until
    /// stop() is called from another thread.
    std::size_t expected_sessions{0};
    /// Optional durable store directory. When empty the coordinator keeps its
    /// committed state in memory only.
    std::filesystem::path store_directory{};
    /// Policy the coordinator governs with. The coordinator owns the policy; a
    /// worker never pushes one, so a worker cannot widen its own authority.
    PolicyDocument policy{};
    /// Capacity and path evidence the coordinator holds on behalf of the fabric.
    std::vector<ResourceCapacity> capacities{};
    std::vector<PathDescriptor> paths{};
};

class Coordinator {
public:
    static StatusOr<std::unique_ptr<Coordinator>> create(const CoordinatorConfig& config);

    ~Coordinator();
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    /// Bind the loopback listener on an ephemeral port.
    Status listen();
    [[nodiscard]] u16 port() const noexcept { return port_; }

    /// Serve sessions. Returns once expected_sessions have completed, or once
    /// stop() has been requested.
    ///
    /// run() owns the coordinator's session and decision state for its whole
    /// duration. Accessors such as sessions(), workers() and governor() must not
    /// be called from another thread while run() is executing; observe them
    /// after it returns. stop() is the one exception and is safe from any thread.
    Status run();

    /// Request termination. Safe to call from another thread.
    Status stop();

    [[nodiscard]] const std::vector<SessionOutcome>& sessions() const noexcept { return sessions_; }
    [[nodiscard]] const std::vector<WorkerRecord>& workers() const noexcept { return workers_; }
    [[nodiscard]] const Governor& governor() const noexcept { return *governor_; }
    /// Mutable access for the owning application, for example to run the final
    /// sweep that withdraws authority whose evidence aged out.
    [[nodiscard]] Governor& governor() noexcept { return *governor_; }
    [[nodiscard]] const RecoveryReport* recovery() const noexcept {
        return store_ != nullptr ? &store_->recovery() : nullptr;
    }
    [[nodiscard]] EpochId epoch() const noexcept { return boot_.epoch; }
    [[nodiscard]] BootId boot() const noexcept { return boot_.boot; }
    [[nodiscard]] Tick last_tick() const noexcept { return last_tick_; }

    /// Stable digest of everything the coordinator decided, for cross process
    /// determinism comparison.
    [[nodiscard]] StatusOr<u64> decision_digest() const;

    /// Write a machine readable session report. Used by the multiprocess tests.
    Status write_report(const std::filesystem::path& path) const;

    /// Render the coordinator report as deterministic JSON.
    [[nodiscard]] std::string render_report() const;

private:
    explicit Coordinator(CoordinatorConfig config);

    [[nodiscard]] Status serve_one();
    [[nodiscard]] Status handle_hello(TcpStream& stream, Frame& frame, SessionOutcome& outcome,
                                      WorkerRecord*& record, bool& fenced);
    [[nodiscard]] Status handle_stream(TcpStream& stream, SessionOutcome& outcome);
    [[nodiscard]] Status apply_evidence_batch(std::span<const std::byte> payload,
                                              SessionOutcome& outcome, Tick now);
    [[nodiscard]] Status apply_completion(std::span<const std::byte> payload, Tick now);
    [[nodiscard]] WorkerRecord* find_worker(WorkerId id);
    [[nodiscard]] Status fence_session(TcpStream& stream, SessionOutcome& outcome,
                                       FenceReason reason, const std::string& detail);

    CoordinatorConfig config_{};
    BootIdentity boot_{};
    TcpListener listener_{};
    u16 port_{0};
    Governor inline_governor_{};
    std::unique_ptr<DurableGovernor> store_{};
    Governor* governor_{nullptr};
    std::vector<SessionOutcome> sessions_{};
    std::vector<WorkerRecord> workers_{};
    std::vector<u64> seen_nonces_{};
    Tick last_tick_{0};
    u64 session_sequence_{0};
    WorkerId next_worker_id_{};
    bool stop_requested_{false};
    bool listening_{false};
};

struct WorkerConfig {
    u16 port{0};
    BootIdentity boot{};
    WorkerId requested_worker{};
    /// Session nonce. Two registrations from the same worker and boot with the
    /// same nonce are a duplicate and are fenced.
    u64 session_nonce{0};
    std::string label{};
    Tick start_tick{0};
    ClusterLimits limits{};
};

class Worker {
public:
    static StatusOr<std::unique_ptr<Worker>> connect(const WorkerConfig& config);

    ~Worker();
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// Perform the Hello/Welcome handshake. On refusal the frame carries the
    /// fence reason and the session is unusable.
    Status register_session();

    Status heartbeat(Tick now);
    Status submit_evidence(const FlowSample& sample);
    Status complete_flow(FlowId flow, Generation generation, Tick at);
    /// Flush buffered evidence and wait for the acknowledgement.
    Status flush();

    Status bye(const std::string& reason);

    [[nodiscard]] WorkerId worker_id() const noexcept { return assigned_worker_; }
    [[nodiscard]] EpochId epoch() const noexcept { return boot_.epoch; }
    [[nodiscard]] BootId boot() const noexcept { return boot_.boot; }
    [[nodiscard]] bool registered() const noexcept { return registered_; }
    [[nodiscard]] FenceReason last_fence() const noexcept { return last_fence_; }
    [[nodiscard]] const std::string& last_fence_detail() const noexcept { return fence_detail_; }
    [[nodiscard]] u64 frames_sent() const noexcept { return frames_sent_; }
    [[nodiscard]] u64 samples_sent() const noexcept { return samples_sent_; }
    [[nodiscard]] u64 accepted() const noexcept { return accepted_; }
    [[nodiscard]] u64 rejected() const noexcept { return rejected_; }
    [[nodiscard]] u64 duplicates() const noexcept { return duplicates_; }

private:
    explicit Worker(WorkerConfig config);

    [[nodiscard]] Status send(FrameType type, std::span<const std::byte> payload);
    [[nodiscard]] Status flush_internal();

    WorkerConfig config_{};
    BootIdentity boot_{};
    TcpStream stream_{};
    FrameCodec codec_{};
    WorkerId assigned_worker_{};
    std::vector<FlowSample> buffer_{};
    u64 sequence_{0};
    u64 frames_sent_{0};
    u64 samples_sent_{0};
    u64 accepted_{0};
    u64 duplicates_{0};
    u64 rejected_{0};
    bool registered_{false};
    FenceReason last_fence_{FenceReason::None};
    std::string fence_detail_{};
};

/// Encode an evidence batch payload.
[[nodiscard]] Status encode_evidence_batch(std::span<const FlowSample> samples,
                                           std::size_t max_batch, ByteWriter& writer);
/// Decode an evidence batch payload.
[[nodiscard]] Status decode_evidence_batch(ByteReader& reader, std::size_t max_batch,
                                           std::vector<FlowSample>& out);

}  // namespace efg

#endif  // EFG_CLUSTER_HPP
