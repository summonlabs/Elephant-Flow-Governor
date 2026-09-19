// Elephant Flow Governor - cluster coordinator and worker implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/cluster.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "efg/codec.hpp"
#include "efg/text.hpp"

namespace efg {
namespace {

constexpr std::size_t kMaxNonceHistory = 4096;
constexpr char kNewline = static_cast<char>(10);

Status read_fence(std::span<const std::byte> payload, FenceReason& reason, std::string& detail) {
    ByteReader reader(payload);
    u16 raw = 0;
    if (!reader.read_u16(raw)) {
        return make_status(StatusCode::Truncated, "fence frame is truncated");
    }
    reason = static_cast<FenceReason>(raw);
    if (!reader.read_string(limits::kMaxNameBytes, detail)) {
        return make_status(StatusCode::Truncated, "fence detail is truncated");
    }
    return {};
}

void append_fence(ByteWriter& writer, FenceReason reason, std::string_view detail) {
    writer.write_u16(static_cast<u16>(reason));
    writer.write_string(detail);
}

}  // namespace

Status encode_evidence_batch(std::span<const FlowSample> samples, std::size_t max_batch,
                             ByteWriter& writer) {
    if (samples.size() > max_batch) {
        return make_status(StatusCode::Oversized, "evidence batch exceeds the configured ceiling");
    }
    writer.write_u32(static_cast<u32>(samples.size()));
    writer.write_u64(0);  // reserved flags/ordering field, always zero
    for (const FlowSample& sample : samples) {
        encode(writer, sample);
    }
    if (writer.overflowed()) {
        return make_status(StatusCode::Oversized, "encoded evidence batch exceeded the ceiling");
    }
    return {};
}

Status decode_evidence_batch(ByteReader& reader, std::size_t max_batch,
                             std::vector<FlowSample>& out) {
    u32 count = 0;
    u64 reserved = 0;
    if (!reader.read_u32(count) || !reader.read_u64(reserved)) {
        return make_status(StatusCode::Truncated, "evidence batch header is truncated");
    }
    if (reserved != 0) {
        return make_status(StatusCode::InvalidArgument, "evidence batch reserved field is not zero");
    }
    if (static_cast<std::size_t>(count) > max_batch) {
        return make_status(StatusCode::Oversized, "evidence batch exceeds the configured ceiling");
    }
    out.clear();
    out.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        FlowSample sample;
        EFG_TRY(decode(reader, sample));
        out.push_back(std::move(sample));
    }
    if (!reader.at_end()) {
        return make_status(StatusCode::ContradictoryEvidence,
                           "evidence batch carries trailing bytes");
    }
    return {};
}

// --- Coordinator ------------------------------------------------------------

Coordinator::Coordinator(CoordinatorConfig config)
    : config_(std::move(config)), boot_(config_.boot), inline_governor_(config_.governor) {}

Coordinator::~Coordinator() {
    if (listening_) {
        (void)listener_.close();
    }
}

StatusOr<std::unique_ptr<Coordinator>> Coordinator::create(const CoordinatorConfig& config) {
    if (!config.boot.valid()) {
        return make_status(StatusCode::InvalidArgument, "coordinator requires a boot identity");
    }
    if (config.limits.max_workers == 0 || config.limits.max_workers > limits::kMaxWorkers) {
        return make_status(StatusCode::OutOfRange, "worker ceiling is outside the supported range");
    }
    if (config.limits.max_payload == 0 ||
        config.limits.max_payload > limits::kMaxFramePayload) {
        return make_status(StatusCode::OutOfRange, "frame ceiling is outside the supported range");
    }
    std::unique_ptr<Coordinator> coordinator{new Coordinator(config)};

    if (!config.store_directory.empty()) {
        DurableGovernor::Options options;
        options.directory = config.store_directory;
        StatusOr<DurableGovernor> store = DurableGovernor::open(options, config.governor,
                                                               config.boot, config.start_tick);
        if (!store.ok()) {
            return store.status();
        }
        coordinator->store_ = std::make_unique<DurableGovernor>(std::move(store.value()));
        coordinator->governor_ = &coordinator->store_->governor();
    } else {
        coordinator->inline_governor_.set_incarnation(config.boot);
        coordinator->governor_ = &coordinator->inline_governor_;
    }
    coordinator->last_tick_ = config.start_tick;
    coordinator->next_worker_id_ = WorkerId{0};

    if (!config.policy.enter_rule.empty()) {
        Status status = coordinator->store_ != nullptr
                            ? coordinator->store_->record_policy(config.policy)
                            : coordinator->governor_->submit_policy(config.policy);
        if (!status.ok()) {
            return status;
        }
    }
    for (const ResourceCapacity& capacity : config.capacities) {
        Status status = coordinator->store_ != nullptr
                            ? coordinator->store_->record_capacity(capacity)
                            : coordinator->governor_->submit_capacity(capacity);
        if (!status.ok()) {
            return status;
        }
    }
    for (const PathDescriptor& path : config.paths) {
        Status status = coordinator->store_ != nullptr
                            ? coordinator->store_->record_path(path)
                            : coordinator->governor_->submit_path(path);
        if (!status.ok()) {
            return status;
        }
    }
    return coordinator;
}

Status Coordinator::listen() {
    if (listening_) {
        return make_status(StatusCode::AlreadyExists, "coordinator is already listening");
    }
    StatusOr<TcpListener> listener = TcpListener::bind_loopback(0);
    if (!listener.ok()) {
        return listener.status();
    }
    listener_ = std::move(listener.value());
    port_ = listener_.bound_port();
    listening_ = true;
    return {};
}

Status Coordinator::stop() {
    stop_requested_ = true;
    if (listening_) {
        (void)listener_.close();
        listening_ = false;
    }
    return {};
}

WorkerRecord* Coordinator::find_worker(WorkerId id) {
    for (WorkerRecord& record : workers_) {
        if (record.worker == id) {
            return &record;
        }
    }
    return nullptr;
}

Status Coordinator::fence_session(TcpStream& stream, SessionOutcome& outcome, FenceReason reason,
                                  const std::string& detail) {
    outcome.fenced = true;
    outcome.fence = reason;
    outcome.detail = detail;
    ByteWriter writer(256);
    append_fence(writer, reason, detail);
    Frame frame;
    frame.header.type = FrameType::Fence;
    frame.header.epoch = boot_.epoch;
    frame.header.boot = boot_.boot;
    frame.header.worker = outcome.worker;
    frame.header.sequence = ++session_sequence_;
    frame.payload = writer.take();
    FrameCodec codec(FrameCodec::Limits{config_.limits.max_payload});
    (void)write_frame(stream, codec, frame);
    (void)stream.shutdown_send();
    return {};
}

Status Coordinator::handle_hello(TcpStream& stream, Frame& frame, SessionOutcome& outcome,
                                 WorkerRecord*& record, bool& fenced) {
    fenced = false;
    ByteReader reader(frame.payload);
    u64 requested_worker = 0;
    u64 boot = 0;
    u64 epoch = 0;
    u64 nonce = 0;
    std::string label;
    if (!reader.read_u64(requested_worker) || !reader.read_u64(boot) || !reader.read_u64(epoch) ||
        !reader.read_u64(nonce) || !reader.read_string(limits::kMaxNameBytes, label)) {
        EFG_TRY(fence_session(stream, outcome, FenceReason::FrameMalformed,
                              "hello payload is truncated"));
        fenced = true;
        return {};
    }
    if (frame.header.epoch != EpochId{epoch} || frame.header.boot != BootId{boot}) {
        EFG_TRY(fence_session(stream, outcome, FenceReason::FrameMalformed,
                              "hello frame identity disagrees with its payload"));
        fenced = true;
        return {};
    }
    outcome.epoch = EpochId{epoch};
    outcome.boot = BootId{boot};
    outcome.label = label;

    if (EpochId{epoch} < boot_.epoch) {
        EFG_TRY(fence_session(stream, outcome, FenceReason::EpochStale,
                              "worker epoch is older than the coordinator epoch"));
        fenced = true;
        return {};
    }
    if (EpochId{epoch} > boot_.epoch) {
        EFG_TRY(fence_session(stream, outcome, FenceReason::EpochAhead,
                              "worker epoch is newer than the coordinator epoch"));
        fenced = true;
        return {};
    }

    const auto seen = std::find(seen_nonces_.begin(), seen_nonces_.end(), nonce);
    if (seen != seen_nonces_.end()) {
        EFG_TRY(fence_session(stream, outcome, FenceReason::WorkerDuplicate,
                              "session nonce was already consumed"));
        fenced = true;
        return {};
    }

    WorkerId assigned{requested_worker};
    if (!assigned.valid()) {
        if (next_worker_id_.value() >= static_cast<u64>(config_.limits.max_workers)) {
            EFG_TRY(fence_session(stream, outcome, FenceReason::PopulationCeiling,
                                  "worker population ceiling reached"));
            fenced = true;
            return {};
        }
        next_worker_id_ = WorkerId{next_worker_id_.value() + 1};
        assigned = next_worker_id_;
    } else if (assigned.value() > static_cast<u64>(config_.limits.max_workers)) {
        EFG_TRY(fence_session(stream, outcome, FenceReason::PopulationCeiling,
                              "requested worker identifier exceeds the ceiling"));
        fenced = true;
        return {};
    } else if (assigned.value() > next_worker_id_.value()) {
        next_worker_id_ = assigned;
    }

    record = find_worker(assigned);
    if (record == nullptr) {
        WorkerRecord fresh;
        fresh.worker = assigned;
        fresh.boot = BootId{boot};
        fresh.epoch = EpochId{epoch};
        fresh.label = label;
        fresh.last_seen = last_tick_;
        workers_.push_back(fresh);
        record = &workers_.back();
    } else {
        if (record->boot != BootId{boot}) {
            // A new incarnation of the same worker supersedes the old one. The
            // superseded incarnation keeps no authority.
            record->outcome = "superseded_by_new_boot";
            record->fences += 1;
        }
        record->boot = BootId{boot};
        record->epoch = EpochId{epoch};
        record->label = label;
        record->last_seen = last_tick_;
    }
    record->sessions += 1;

    if (seen_nonces_.size() >= kMaxNonceHistory) {
        seen_nonces_.erase(seen_nonces_.begin());
    }
    seen_nonces_.push_back(nonce);

    outcome.worker = assigned;
    outcome.label = label;

    ByteWriter writer(256);
    writer.write_u64(assigned.value());
    writer.write_u64(boot_.epoch.value());
    writer.write_u64(boot_.boot.value());
    const PolicyDocument* policy = governor_->policy();
    writer.write_u64(policy != nullptr ? policy->digest : 0);
    writer.write_u32(1);  // accepted

    Frame welcome;
    welcome.header.type = FrameType::Welcome;
    welcome.header.epoch = boot_.epoch;
    welcome.header.boot = boot_.boot;
    welcome.header.worker = assigned;
    welcome.header.sequence = ++session_sequence_;
    welcome.payload = writer.take();
    FrameCodec codec(FrameCodec::Limits{config_.limits.max_payload});
    return write_frame(stream, codec, welcome);
}

Status Coordinator::apply_evidence_batch(std::span<const std::byte> payload,
                                         SessionOutcome& outcome, Tick now) {
    ByteReader reader(payload);
    std::vector<FlowSample> samples;
    EFG_TRY(decode_evidence_batch(reader, config_.limits.max_batch, samples));
    for (const FlowSample& sample : samples) {
        ++outcome.samples;
        Tick decision_tick = sample.span.end;
        if (sample.validity.issued_at > decision_tick) {
            decision_tick = sample.validity.issued_at;
        }
        if (decision_tick < now) {
            decision_tick = now;
        }
        StatusOr<FlowDecision> decision = store_ != nullptr
                                              ? store_->record_evidence(sample)
                                              : governor_->submit_evidence(sample);
        if (decision.ok()) {
            ++outcome.accepted;
            if (decision.value().evidence_duplicate) {
                ++outcome.duplicates;
            }
            if (decision_tick > last_tick_) {
                last_tick_ = decision_tick;
            }
        } else {
            ++outcome.rejected;
        }
    }
    return {};
}

Status Coordinator::apply_completion(std::span<const std::byte> payload, Tick now) {
    ByteReader reader(payload);
    u64 flow = 0;
    u64 generation = 0;
    u64 at = 0;
    if (!reader.read_u64(flow) || !reader.read_u64(generation) || !reader.read_u64(at)) {
        return make_status(StatusCode::Truncated, "completion frame is truncated");
    }
    if (!FlowId{flow}.valid() || !Generation{generation}.valid()) {
        return make_status(StatusCode::InvalidArgument, "completion identity is invalid");
    }
    if (at < now) {
        at = now;
    }
    const Status status = governor_->complete_flow(FlowId{flow}, Generation{generation}, at);
    if (!status.ok() && status.code() != StatusCode::NotFound) {
        return status;
    }
    if (at > last_tick_) {
        last_tick_ = at;
    }
    return {};
}

Status Coordinator::handle_stream(TcpStream& stream, SessionOutcome& outcome) {
    FrameCodec codec(FrameCodec::Limits{config_.limits.max_payload});
    bool clean_bye = false;
    while (!stop_requested_) {
        Frame frame;
        const Status received = read_frame(stream, codec, frame);
        if (!received.ok()) {
            if (received.code() == StatusCode::Closed) {
                outcome.detail = "peer closed the connection";
                break;
            }
            EFG_TRY(fence_session(stream, outcome, FenceReason::FrameMalformed, "frame read failed"));
            break;
        }
        ++outcome.frames;
        if (frame.header.epoch != boot_.epoch) {
            EFG_TRY(fence_session(stream, outcome, FenceReason::EpochStale,
                                  "frame epoch disagrees with the coordinator epoch"));
            break;
        }
        if (frame.header.worker != outcome.worker) {
            EFG_TRY(fence_session(stream, outcome, FenceReason::WorkerUnknown,
                                  "frame worker disagrees with the registered worker"));
            break;
        }
        if (frame.header.boot != outcome.boot) {
            EFG_TRY(fence_session(stream, outcome, FenceReason::BootMismatch,
                                  "frame boot disagrees with the registered incarnation"));
            break;
        }
        if (frame.header.type == FrameType::Bye) {
            clean_bye = true;
            break;
        }
        if (frame.header.type == FrameType::Hello) {
            EFG_TRY(fence_session(stream, outcome, FenceReason::WorkerDuplicate,
                                  "second hello inside one session"));
            break;
        }

        switch (frame.header.type) {
            case FrameType::Heartbeat: {
                ByteReader reader(frame.payload);
                u64 tick = 0;
                if (!reader.read_u64(tick)) {
                    EFG_TRY(fence_session(stream, outcome, FenceReason::FrameMalformed,
                                          "heartbeat payload is truncated"));
                    return {};
                }
                if (tick > last_tick_) {
                    last_tick_ = tick;
                }
                break;
            }
            case FrameType::EvidenceBatch: {
                const Status applied = apply_evidence_batch(frame.payload, outcome, last_tick_);
                if (!applied.ok()) {
                    ByteWriter writer(256);
                    append_fence(writer, applied.code() == StatusCode::Oversized
                                             ? FenceReason::FrameOversized
                                             : FenceReason::FrameMalformed,
                                 applied.message());
                    Frame reply;
                    reply.header.type = FrameType::Fence;
                    reply.header.epoch = boot_.epoch;
                    reply.header.boot = boot_.boot;
                    reply.header.worker = outcome.worker;
                    reply.header.sequence = ++session_sequence_;
                    reply.payload = writer.take();
                    (void)write_frame(stream, codec, reply);
                    outcome.fenced = true;
                    outcome.fence = applied.code() == StatusCode::Oversized
                                        ? FenceReason::FrameOversized
                                        : FenceReason::FrameMalformed;
                    outcome.detail = std::string{applied.message()};
                    return {};
                }
                ByteWriter writer(256);
                writer.write_u64(outcome.accepted);
                writer.write_u64(outcome.duplicates);
                writer.write_u64(outcome.rejected);
                writer.write_u64(last_tick_);
                const StatusOr<u64> digest = decision_digest();
                writer.write_u64(digest.ok() ? digest.value() : 0);

                Frame ack;
                ack.header.type = FrameType::EvidenceAck;
                ack.header.epoch = boot_.epoch;
                ack.header.boot = boot_.boot;
                ack.header.worker = outcome.worker;
                ack.header.sequence = ++session_sequence_;
                ack.payload = writer.take();
                EFG_TRY(write_frame(stream, codec, ack));
                break;
            }
            case FrameType::FlowCompletion: {
                const Status applied = apply_completion(frame.payload, last_tick_);
                if (!applied.ok()) {
                    EFG_TRY(fence_session(stream, outcome, FenceReason::GenerationRegression,
                                          std::string{applied.message()}));
                    return {};
                }
                break;
            }
            case FrameType::Shutdown:
                clean_bye = true;
                break;
            case FrameType::Invalid:
            case FrameType::Hello:
            case FrameType::Welcome:
            case FrameType::Fence:
            case FrameType::EvidenceAck:
            case FrameType::PolicyPush:
            case FrameType::CapacityPush:
            case FrameType::PathPush:
            case FrameType::IntentGrant:
            case FrameType::StatusReport:
            case FrameType::Bye:
            case FrameType::Failure:
            case FrameType::Count:
            default:
                EFG_TRY(fence_session(stream, outcome, FenceReason::FrameMalformed,
                                      "frame type is not accepted from a worker"));
                return {};
        }
    }
    outcome.clean_bye = clean_bye;
    return {};
}

Status Coordinator::serve_one() {
    StatusOr<TcpStream> accepted = listener_.accept();
    if (!accepted.ok()) {
        return accepted.status();
    }
    TcpStream stream = std::move(accepted.value());
    SessionOutcome outcome;
    outcome.detail = "session opened";

    FrameCodec codec(FrameCodec::Limits{config_.limits.max_payload});
    Frame hello;
    Status received = read_frame(stream, codec, hello);
    if (!received.ok()) {
        outcome.fenced = true;
        outcome.fence = FenceReason::FrameMalformed;
        outcome.detail = std::string{"hello was not readable: "} + std::string{received.message()};
        sessions_.push_back(std::move(outcome));
        return {};
    }
    ++outcome.frames;
    if (hello.header.type != FrameType::Hello) {
        EFG_TRY(fence_session(stream, outcome, FenceReason::FrameMalformed,
                              "first frame was not a hello"));
        sessions_.push_back(std::move(outcome));
        return {};
    }

    WorkerRecord* record = nullptr;
    bool fenced = false;
    const Status handled = handle_hello(stream, hello, outcome, record, fenced);
    if (!handled.ok() || fenced) {
        if (record != nullptr) {
            record->last_seen = last_tick_;
            record->outcome = std::string{"fenced: "} + std::string{to_string(outcome.fence)};
        }
        sessions_.push_back(std::move(outcome));
        return {};
    }

    const Status streamed = handle_stream(stream, outcome);
    if (!streamed.ok()) {
        outcome.detail = std::string{streamed.message()};
    }
    if (record != nullptr) {
        record->last_seen = last_tick_;
        record->outcome = outcome.fenced ? std::string{"fenced: "} + std::string{to_string(outcome.fence)}
                                         : (outcome.clean_bye ? "clean" : "closed");
        if (outcome.fenced) {
            record->fences += 1;
        }
    }
    (void)stream.shutdown_send();
    (void)stream.close();
    sessions_.push_back(std::move(outcome));
    return {};
}

Status Coordinator::run() {
    if (!listening_) {
        EFG_TRY(listen());
    }
    while (!stop_requested_) {
        if (config_.expected_sessions != 0 && sessions_.size() >= config_.expected_sessions) {
            break;
        }
        const Status served = serve_one();
        if (!served.ok()) {
            if (stop_requested_ || !listening_) {
                break;
            }
            if (served.code() == StatusCode::Closed) {
                break;
            }
            return served;
        }
    }
    return {};
}

StatusOr<u64> Coordinator::decision_digest() const { return efg::decision_digest(*governor_); }

std::string Coordinator::render_report() const {
    std::string out;
    out.reserve(2048);
    out.append("{");
    out.push_back(kNewline);
    out.append("  \"coordinator_boot\": ");
    append_u64(out, boot_.boot.value());
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"coordinator_epoch\": ");
    append_u64(out, boot_.epoch.value());
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"port\": ");
    append_u64(out, port_);
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"last_tick\": ");
    append_u64(out, last_tick_);
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"sessions\": [");
    for (std::size_t i = 0; i < sessions_.size(); ++i) {
        const SessionOutcome& session = sessions_[i];
        out.push_back(kNewline);
        out.append(i == 0 ? "    {" : "    ,{");
        out.append("\"worker\": ");
        append_u64(out, session.worker.value());
        out.append(", \"boot\": ");
        append_u64(out, session.boot.value());
        out.append(", \"epoch\": ");
        append_u64(out, session.epoch.value());
        out.append(", \"fenced\": ");
        out.append(session.fenced ? "true" : "false");
        out.append(", \"fence\": ");
        append_json_string(out, to_string(session.fence));
        out.append(", \"clean_bye\": ");
        out.append(session.clean_bye ? "true" : "false");
        out.append(", \"frames\": ");
        append_u64(out, session.frames);
        out.append(", \"samples\": ");
        append_u64(out, session.samples);
        out.append(", \"accepted\": ");
        append_u64(out, session.accepted);
        out.append(", \"duplicates\": ");
        append_u64(out, session.duplicates);
        out.append(", \"rejected\": ");
        append_u64(out, session.rejected);
        out.append(", \"label\": ");
        append_json_string(out, session.label);
        out.append(", \"detail\": ");
        append_json_string(out, session.detail);
        out.push_back('}');
    }
    if (!sessions_.empty()) {
        out.push_back(kNewline);
        out.append("  ");
    }
    out.append("],");
    out.push_back(kNewline);
    out.append("  \"decisions\": ");
    append_u64(out, governor_->counters().decisions);
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"elephant_decisions\": ");
    append_u64(out, governor_->counters().elephant_decisions);
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"suspended_decisions\": ");
    append_u64(out, governor_->counters().suspended_decisions);
    out.append(",");
    out.push_back(kNewline);
    out.append("  \"intents_fenced\": ");
    append_u64(out, governor_->counters().intents_fenced);
    out.push_back(kNewline);
    out.append("}");
    out.push_back(kNewline);
    return out;
}
Status Coordinator::write_report(const std::filesystem::path& path) const {
    const std::string json = render_report();
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        return make_status(StatusCode::IoError, "coordinator report could not be created");
    }
    const std::size_t written = std::fwrite(json.data(), 1, json.size(), file);
    std::fclose(file);
    if (written != json.size()) {
        return make_status(StatusCode::IoError, "coordinator report write failed");
    }
    return {};
}

// --- Worker -----------------------------------------------------------------

Worker::Worker(WorkerConfig config) : config_(std::move(config)), boot_(config_.boot) {
    codec_ = FrameCodec(FrameCodec::Limits{config_.limits.max_payload});
}

Worker::~Worker() = default;

StatusOr<std::unique_ptr<Worker>> Worker::connect(const WorkerConfig& config) {
    if (config.port == 0) {
        return make_status(StatusCode::InvalidArgument, "worker requires a coordinator port");
    }
    if (!config.boot.valid()) {
        return make_status(StatusCode::InvalidArgument, "worker requires a boot identity");
    }
    std::unique_ptr<Worker> worker{new Worker(config)};
    StatusOr<TcpStream> stream = connect_loopback(config.port);
    if (!stream.ok()) {
        return stream.status();
    }
    worker->stream_ = std::move(stream.value());
    return worker;
}

Status Worker::send(FrameType type, std::span<const std::byte> payload) {
    if (!stream_.valid()) {
        return make_status(StatusCode::Closed, "worker stream is closed");
    }
    Frame frame;
    frame.header.type = type;
    frame.header.epoch = boot_.epoch;
    frame.header.boot = boot_.boot;
    frame.header.worker = assigned_worker_;
    frame.header.sequence = ++sequence_;
    frame.payload.assign(payload.begin(), payload.end());
    EFG_TRY(write_frame(stream_, codec_, frame));
    ++frames_sent_;
    return {};
}

Status Worker::register_session() {
    ByteWriter writer(256);
    writer.write_u64(config_.requested_worker.value());
    writer.write_u64(boot_.boot.value());
    writer.write_u64(boot_.epoch.value());
    writer.write_u64(config_.session_nonce);
    writer.write_string(config_.label);
    if (writer.overflowed()) {
        return make_status(StatusCode::Oversized, "hello payload exceeded the ceiling");
    }
    EFG_TRY(send(FrameType::Hello, writer.buffer()));

    Frame reply;
    EFG_TRY(read_frame(stream_, codec_, reply));
    if (reply.header.type == FrameType::Fence) {
        FenceReason reason = FenceReason::None;
        std::string detail;
        (void)read_fence(reply.payload, reason, detail);
        last_fence_ = reason;
        fence_detail_ = detail;
        return make_status(StatusCode::AuthorityMismatch,
                           "coordinator fenced the registration");
    }
    if (reply.header.type != FrameType::Welcome) {
        return make_status(StatusCode::Conflict, "coordinator did not accept the registration");
    }
    ByteReader reader(reply.payload);
    u64 worker = 0;
    u64 epoch = 0;
    u64 coordinator_boot = 0;
    u64 policy_digest = 0;
    u32 accepted = 0;
    if (!reader.read_u64(worker) || !reader.read_u64(epoch) || !reader.read_u64(coordinator_boot) ||
        !reader.read_u64(policy_digest) || !reader.read_u32(accepted)) {
        return make_status(StatusCode::Truncated, "welcome payload is truncated");
    }
    if (accepted != 1) {
        return make_status(StatusCode::AuthorityMismatch, "coordinator refused the session");
    }
    if (EpochId{epoch} != boot_.epoch) {
        last_fence_ = FenceReason::EpochAhead;
        fence_detail_ = "welcome epoch disagrees with the worker epoch";
        return make_status(StatusCode::EpochMismatch, "welcome epoch disagrees with the worker epoch");
    }
    if (reply.header.boot != BootId{coordinator_boot}) {
        return make_status(StatusCode::BootMismatch, "welcome boot disagrees with the frame boot");
    }
    assigned_worker_ = WorkerId{worker};
    registered_ = true;
    return {};
}

Status Worker::heartbeat(Tick now) {
    if (!registered_) {
        return make_status(StatusCode::AuthorityMismatch, "worker is not registered");
    }
    ByteWriter writer(16);
    writer.write_u64(now);
    return send(FrameType::Heartbeat, writer.buffer());
}

Status Worker::submit_evidence(const FlowSample& sample) {
    if (!registered_) {
        return make_status(StatusCode::AuthorityMismatch, "worker is not registered");
    }
    if (buffer_.size() >= config_.limits.max_batch) {
        EFG_TRY(flush_internal());
    }
    buffer_.push_back(sample);
    return {};
}

Status Worker::complete_flow(FlowId flow, Generation generation, Tick at) {
    if (!registered_) {
        return make_status(StatusCode::AuthorityMismatch, "worker is not registered");
    }
    EFG_TRY(flush_internal());
    ByteWriter writer(32);
    writer.write_u64(flow.value());
    writer.write_u64(generation.value());
    writer.write_u64(at);
    return send(FrameType::FlowCompletion, writer.buffer());
}

Status Worker::flush_internal() {
    if (buffer_.empty()) {
        return {};
    }
    ByteWriter writer(limits::kMaxFramePayload);
    EFG_TRY(encode_evidence_batch(buffer_, config_.limits.max_batch, writer));
    const std::size_t count = buffer_.size();
    EFG_TRY(send(FrameType::EvidenceBatch, writer.buffer()));
    samples_sent_ += count;
    buffer_.clear();

    Frame ack;
    EFG_TRY(read_frame(stream_, codec_, ack));
    if (ack.header.type == FrameType::Fence) {
        FenceReason reason = FenceReason::None;
        std::string detail;
        (void)read_fence(ack.payload, reason, detail);
        last_fence_ = reason;
        fence_detail_ = detail;
        return make_status(StatusCode::AuthorityMismatch, "coordinator fenced the evidence batch");
    }
    if (ack.header.type != FrameType::EvidenceAck) {
        return make_status(StatusCode::Conflict, "coordinator did not acknowledge the batch");
    }
    ByteReader reader(ack.payload);
    u64 digest = 0;
    u64 tick = 0;
    if (!reader.read_u64(accepted_) || !reader.read_u64(duplicates_) || !reader.read_u64(rejected_) ||
        !reader.read_u64(tick) || !reader.read_u64(digest)) {
        return make_status(StatusCode::Truncated, "acknowledgement payload is truncated");
    }
    return {};
}

Status Worker::flush() {
    if (!registered_) {
        return make_status(StatusCode::AuthorityMismatch, "worker is not registered");
    }
    return flush_internal();
}

Status Worker::bye(const std::string& reason) {
    if (!registered_) {
        return make_status(StatusCode::AuthorityMismatch, "worker is not registered");
    }
    EFG_TRY(flush_internal());
    ByteWriter writer(256);
    writer.write_u16(0);
    writer.write_string(reason);
    EFG_TRY(send(FrameType::Bye, writer.buffer()));
    registered_ = false;
    (void)stream_.shutdown_send();
    return {};
}

}  // namespace efg
