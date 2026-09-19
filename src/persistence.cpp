// Elephant Flow Governor - durable state implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/persistence.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>

#include "efg/codec.hpp"
#include "efg/text.hpp"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace efg {
namespace {

constexpr char kJournalMagic[8] = {'E', 'F', 'G', 'J', 'R', 'N', 'L', '1'};
constexpr u16 kRecordMagic = 0xEF6Au;
constexpr std::size_t kJournalHeaderBytes = 16;
constexpr std::size_t kRecordHeaderBytes = 20;

std::FILE* as_file(void* handle) { return static_cast<std::FILE*>(handle); }

Status flush_to_stable(std::FILE* file) {
    if (std::fflush(file) != 0) {
        return make_status(StatusCode::IoError, "flush failed");
    }
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        return make_status(StatusCode::IoError, "commit to stable storage failed");
    }
#else
    if (::fsync(fileno(file)) != 0) {
        return make_status(StatusCode::IoError, "fsync failed");
    }
#endif
    return {};
}

Status rename_over(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
        0) {
        return make_status(StatusCode::IoError, "atomic rename failed");
    }
    return {};
#else
    std::error_code error;
    std::filesystem::rename(from, to, error);
    if (error) {
        return make_status(StatusCode::IoError, "atomic rename failed");
    }
    return {};
#endif
}

}  // namespace

std::string_view to_string(RecordType type) noexcept {
    switch (type) {
        case RecordType::Begin: return "begin";
        case RecordType::PolicyAccepted: return "policy_accepted";
        case RecordType::CapacityAccepted: return "capacity_accepted";
        case RecordType::PathAccepted: return "path_accepted";
        case RecordType::FlowClassified: return "flow_classified";
        case RecordType::FlowTransition: return "flow_transition";
        case RecordType::IntentRecorded: return "intent_recorded";
        case RecordType::IntentTransition: return "intent_transition";
        case RecordType::IntentFenced: return "intent_fenced";
        case RecordType::TickCommitted: return "tick_committed";
        case RecordType::Snapshot: return "snapshot";
        case RecordType::Shutdown: return "shutdown";
        case RecordType::Count: return "count";
    }
    return "begin";
}

std::string_view to_string(RecoveryDisposition disposition) noexcept {
    switch (disposition) {
        case RecoveryDisposition::DurableConfiguration: return "durable_configuration";
        case RecoveryDisposition::CommittedState: return "committed_state";
        case RecoveryDisposition::UnfinishedAttempt: return "unfinished_attempt";
        case RecoveryDisposition::AmbiguousOutcome: return "ambiguous_outcome";
        case RecoveryDisposition::StaleLiveAuthority: return "stale_live_authority";
        case RecoveryDisposition::EvidenceRequiresRevalidation: return "evidence_requires_revalidation";
    }
    return "durable_configuration";
}

Status RecoveryReport::validate() const {
    if (records_replayed + records_rejected > limits::kMaxJournalReplayRecords) {
        return make_status(StatusCode::Oversized, "replay counters exceed the supported range");
    }
    if (findings.size() > 64) {
        return make_status(StatusCode::Oversized, "recovery findings exceed the report ceiling");
    }
    return {};
}

// --- Journal ----------------------------------------------------------------

Journal::~Journal() {
    if (is_open()) {
        (void)close();
    }
}

Journal::Journal(Journal&& other) noexcept
    : path_(std::move(other.path_)), file_(other.file_), options_(other.options_),
      sequence_(other.sequence_), size_bytes_(other.size_bytes_) {
    other.file_ = nullptr;
    other.sequence_ = 0;
    other.size_bytes_ = 0;
}

Journal& Journal::operator=(Journal&& other) noexcept {
    if (this != &other) {
        if (is_open()) {
            (void)close();
        }
        path_ = std::move(other.path_);
        file_ = other.file_;
        options_ = other.options_;
        sequence_ = other.sequence_;
        size_bytes_ = other.size_bytes_;
        other.file_ = nullptr;
        other.sequence_ = 0;
        other.size_bytes_ = 0;
    }
    return *this;
}

StatusOr<Journal> Journal::open(const std::filesystem::path& path, Options options) {
    if (path.empty()) {
        return make_status(StatusCode::InvalidArgument, "journal path is empty");
    }
    if (options.max_record_bytes == 0 ||
        options.max_record_bytes > limits::kMaxJournalRecordBytes) {
        return make_status(StatusCode::OutOfRange, "journal record ceiling is outside the range");
    }

    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return make_status(StatusCode::IoError, "journal directory could not be created");
        }
    }

    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return make_status(StatusCode::IoError, "journal existence could not be determined");
    }

    Journal journal;
    journal.path_ = path;
    journal.options_ = options;

    if (!exists) {
        std::FILE* created = std::fopen(path.string().c_str(), "wb");
        if (created == nullptr) {
            return make_status(StatusCode::IoError, "journal could not be created");
        }
        ByteWriter header(kJournalHeaderBytes);
        header.write_bytes(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(kJournalMagic), sizeof(kJournalMagic)));
        header.write_u32(kSchemaVersion);
        const u32 header_crc = crc32c(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(header.buffer().data()), 12));
        header.write_u32(header_crc);
        const std::size_t written =
            std::fwrite(header.buffer().data(), 1, header.buffer().size(), created);
        const Status status = written == header.buffer().size()
                                  ? flush_to_stable(created)
                                  : make_status(StatusCode::IoError, "journal header write failed");
        std::fclose(created);
        if (!status.ok()) {
            return status;
        }
    } else {
        // Validate the existing header. A damaged header is refused, never
        // repaired: the operator is told rather than silently losing history.
        std::FILE* existing = std::fopen(path.string().c_str(), "rb");
        if (existing == nullptr) {
            return make_status(StatusCode::IoError, "existing journal could not be opened");
        }
        std::byte buffer[kJournalHeaderBytes];
        const std::size_t read = std::fread(buffer, 1, sizeof(buffer), existing);
        std::fclose(existing);
        if (read != sizeof(buffer)) {
            return make_status(StatusCode::Truncated, "journal header is truncated");
        }
        if (std::memcmp(buffer, kJournalMagic, sizeof(kJournalMagic)) != 0) {
            return make_status(StatusCode::Corrupt, "journal magic does not match");
        }
        const u32 crc = crc32c(std::span<const std::byte>(buffer, 12));
        u32 stored_crc = 0;
        std::memcpy(&stored_crc, buffer + 12, sizeof(stored_crc));
        if (crc != stored_crc) {
            return make_status(StatusCode::IntegrityFailure, "journal header checksum does not match");
        }
        u32 schema = 0;
        std::memcpy(&schema, buffer + 8, sizeof(schema));
        if (schema != kSchemaVersion) {
            return make_status(StatusCode::VersionMismatch, "journal schema version is not supported");
        }
    }

    std::FILE* append = std::fopen(path.string().c_str(), "ab");
    if (append == nullptr) {
        return make_status(StatusCode::IoError, "journal could not be opened for append");
    }
    journal.file_ = append;
    const auto size = std::filesystem::file_size(path, error);
    journal.size_bytes_ = error ? 0 : static_cast<u64>(size);

    // Recover the sequence counter from the intact prefix.
    JournalReplayStats stats;
    (void)Journal::verify(path, stats);
    journal.sequence_ = stats.sequence_last;
    return journal;
}

Status Journal::append(RecordType type, std::span<const std::byte> payload, bool durable) {
    if (!is_open()) {
        return make_status(StatusCode::Closed, "journal is not open");
    }
    if (type >= RecordType::Count) {
        return make_status(StatusCode::InvalidArgument, "record type is not appendable");
    }
    if (payload.size() > options_.max_record_bytes) {
        return make_status(StatusCode::Oversized, "journal record exceeds the configured ceiling");
    }
    const u64 record_bytes = kRecordHeaderBytes + payload.size() + 4u;
    if (record_bytes > options_.max_file_bytes ||
        size_bytes_ > options_.max_file_bytes - record_bytes) {
        return make_status(StatusCode::Oversized, "journal file ceiling would be exceeded");
    }
    if (sequence_ == UINT64_MAX) {
        return make_status(StatusCode::Overflow, "journal sequence is exhausted");
    }

    ByteWriter writer(kRecordHeaderBytes);
    writer.write_u16(kRecordMagic);
    writer.write_u16(static_cast<u16>(type));
    writer.write_u32(kSchemaVersion);
    writer.write_u32(static_cast<u32>(payload.size()));
    writer.write_u64(sequence_ + 1);

    const u32 payload_crc = crc32c(payload);
    std::FILE* file = as_file(file_);
    if (std::fwrite(writer.buffer().data(), 1, writer.buffer().size(), file) !=
        writer.buffer().size()) {
        return make_status(StatusCode::IoError, "journal record header write failed");
    }
    if (!payload.empty() &&
        std::fwrite(payload.data(), 1, payload.size(), file) != payload.size()) {
        return make_status(StatusCode::IoError, "journal record payload write failed");
    }
    std::byte crc_bytes[4];
    std::memcpy(crc_bytes, &payload_crc, sizeof(payload_crc));
    if (std::fwrite(crc_bytes, 1, sizeof(crc_bytes), file) != sizeof(crc_bytes)) {
        return make_status(StatusCode::IoError, "journal record checksum write failed");
    }

    ++sequence_;
    size_bytes_ += record_bytes;

    if (durable && options_.fsync_on_commit) {
        return flush_to_stable(file);
    }
    return {};
}

Status Journal::append(RecordType type, std::span<const std::byte> payload) {
    return append(type, payload, true);
}

Status Journal::sync() {
    if (!is_open()) {
        return make_status(StatusCode::Closed, "journal is not open");
    }
    return flush_to_stable(as_file(file_));
}

Status Journal::reset() {
    if (!is_open()) {
        return make_status(StatusCode::Closed, "journal is not open");
    }
    std::FILE* file = as_file(file_);
    if (std::fflush(file) != 0) {
        return make_status(StatusCode::IoError, "journal flush before reset failed");
    }
    std::fclose(file);
    file_ = nullptr;

    std::FILE* truncated = std::fopen(path_.string().c_str(), "wb");
    if (truncated == nullptr) {
        return make_status(StatusCode::IoError, "journal could not be truncated");
    }
    ByteWriter header(kJournalHeaderBytes);
    header.write_bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(kJournalMagic),
                                                  sizeof(kJournalMagic)));
    header.write_u32(kSchemaVersion);
    header.write_u32(crc32c(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(header.buffer().data()), 12)));
    const std::size_t written =
        std::fwrite(header.buffer().data(), 1, header.buffer().size(), truncated);
    Status status = written == header.buffer().size()
                        ? flush_to_stable(truncated)
                        : make_status(StatusCode::IoError, "journal header rewrite failed");
    std::fclose(truncated);
    if (!status.ok()) {
        return status;
    }

    std::FILE* append = std::fopen(path_.string().c_str(), "ab");
    if (append == nullptr) {
        return make_status(StatusCode::IoError, "journal could not be reopened after reset");
    }
    file_ = append;
    sequence_ = 0;
    size_bytes_ = kJournalHeaderBytes;
    return {};
}

Status Journal::close() {
    if (!is_open()) {
        return {};
    }
    std::FILE* file = as_file(file_);
    const Status flushed = flush_to_stable(file);
    const int result = std::fclose(file);
    file_ = nullptr;
    if (!flushed.ok()) {
        return flushed;
    }
    if (result != 0) {
        return make_status(StatusCode::IoError, "journal close failed");
    }
    return {};
}

Status Journal::parse_header(std::span<const std::byte> bytes, std::size_t max_record_bytes,
                             RecordType& type, u32& payload_length, u64& sequence,
                             std::size_t& header_size) {
    header_size = kRecordHeaderBytes;
    if (bytes.size() < kRecordHeaderBytes) {
        return make_status(StatusCode::Truncated, "journal record header is truncated");
    }
    ByteReader reader(bytes.first(kRecordHeaderBytes));
    u16 magic = 0;
    u16 raw_type = 0;
    u32 schema = 0;
    if (!reader.read_u16(magic) || !reader.read_u16(raw_type)) {
        return make_status(StatusCode::Truncated, "journal record prefix is truncated");
    }
    if (magic != kRecordMagic) {
        return make_status(StatusCode::Corrupt, "journal record magic does not match");
    }
    if (raw_type == 0 || raw_type >= static_cast<u16>(RecordType::Count)) {
        return make_status(StatusCode::Corrupt, "journal record type is not recognized");
    }
    if (!reader.read_u32(schema)) {
        return make_status(StatusCode::Truncated, "journal record schema is truncated");
    }
    if (schema != kSchemaVersion) {
        return make_status(StatusCode::VersionMismatch, "journal record schema is not supported");
    }
    if (!reader.read_u32(payload_length) || !reader.read_u64(sequence)) {
        return make_status(StatusCode::Truncated, "journal record body is truncated");
    }
    if (payload_length > max_record_bytes) {
        return make_status(StatusCode::Oversized, "journal record length exceeds the ceiling");
    }
    type = static_cast<RecordType>(raw_type);
    return {};
}

Status Journal::replay(const std::filesystem::path& path,
                       const std::function<Status(RecordType, std::span<const std::byte>, u64)>& visitor,
                       JournalReplayStats& stats) {
    stats = JournalReplayStats{};
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        stats.header_invalid = true;
        return make_status(StatusCode::NotFound, "journal file does not exist");
    }
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return make_status(StatusCode::IoError, "journal could not be opened for replay");
    }
    std::byte header[kJournalHeaderBytes];
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header) ||
        std::memcmp(header, kJournalMagic, sizeof(kJournalMagic)) != 0) {
        std::fclose(file);
        stats.header_invalid = true;
        return make_status(StatusCode::Corrupt, "journal header is invalid");
    }
    u32 stored_crc = 0;
    std::memcpy(&stored_crc, header + 12, sizeof(stored_crc));
    if (crc32c(std::span<const std::byte>(header, 12)) != stored_crc) {
        std::fclose(file);
        stats.header_invalid = true;
        return make_status(StatusCode::IntegrityFailure, "journal header checksum does not match");
    }

    const auto total = std::filesystem::file_size(path, error);
    if (error) {
        std::fclose(file);
        return make_status(StatusCode::IoError, "journal size could not be determined");
    }

    std::vector<std::byte> buffer;
    buffer.resize(64u * 1024u);

    // Records start after the fixed file header. The cursor, not the reported
    // byte count, is the file offset.
    u64 cursor = kJournalHeaderBytes;

    while (true) {
        const std::streamoff offset = static_cast<std::streamoff>(cursor);
        if (cursor + kRecordHeaderBytes + 4u > total) {
            if (cursor != total) {
                stats.truncated_tail = true;
            }
            break;
        }
        if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
            std::fclose(file);
            return make_status(StatusCode::IoError, "journal seek failed");
        }
        const std::size_t header_read = std::fread(buffer.data(), 1, kRecordHeaderBytes, file);
        if (header_read < kRecordHeaderBytes) {
            stats.truncated_tail = true;
            break;
        }
        RecordType type = RecordType::Begin;
        u32 payload_length = 0;
        u64 sequence = 0;
        std::size_t header_size = 0;
        const Status parsed = Journal::parse_header(
            std::span<const std::byte>(buffer.data(), kRecordHeaderBytes),
            limits::kMaxJournalRecordBytes, type, payload_length, sequence, header_size);
        if (!parsed.ok()) {
            if (parsed.code() == StatusCode::Oversized) {
                stats.oversize_record = true;
                std::fclose(file);
                return parsed;
            }
            stats.truncated_tail = true;
            break;
        }
        const u64 record_total = kRecordHeaderBytes + payload_length + 4u;
        if (cursor + record_total > total) {
            stats.truncated_tail = true;
            break;
        }
        if (buffer.size() < payload_length + 4u) {
            buffer.resize(payload_length + 4u);
        }
        const std::size_t body_read =
            std::fread(buffer.data(), 1, payload_length + 4u, file);
        if (body_read < payload_length + 4u) {
            stats.truncated_tail = true;
            break;
        }
        const u32 expected_crc = crc32c(std::span<const std::byte>(buffer.data(), payload_length));
        u32 stored_payload_crc = 0;
        std::memcpy(&stored_payload_crc, buffer.data() + payload_length, sizeof(stored_payload_crc));
        if (expected_crc != stored_payload_crc) {
            // A damaged record in the middle of the file cannot be resynchronised.
            // It is reported as an ambiguous outcome rather than applied.
            ++stats.rejected;
            std::fclose(file);
            return make_status(StatusCode::IntegrityFailure,
                               "journal record checksum does not match");
        }
        const Status applied = visitor(type, std::span<const std::byte>(buffer.data(), payload_length),
                                       sequence);
        if (!applied.ok()) {
            ++stats.rejected;
            std::fclose(file);
            return applied;
        }
        ++stats.records;
        cursor += record_total;
        stats.sequence_last = sequence;
    }

    stats.bytes = cursor - kJournalHeaderBytes;
    std::fclose(file);
    return {};
}

Status Journal::verify(const std::filesystem::path& path, JournalReplayStats& stats) {
    return Journal::replay(path, [](RecordType, std::span<const std::byte>, u64) { return Status{}; },
                           stats);
}

// --- Snapshot ---------------------------------------------------------------

namespace {

constexpr char kSnapshotMagic[8] = {'E', 'F', 'G', 'S', 'N', 'A', 'P', '1'};
constexpr std::size_t kSnapshotHeaderBytes = 24;  // magic 8 + schema 4 + length 8 + crc 4

}  // namespace

Status SnapshotFile::write_atomic(const std::filesystem::path& path,
                                  std::span<const std::byte> payload, Options options) {
    if (path.empty()) {
        return make_status(StatusCode::InvalidArgument, "snapshot path is empty");
    }
    if (payload.size() > options.max_bytes) {
        return make_status(StatusCode::Oversized, "snapshot exceeds the configured ceiling");
    }
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return make_status(StatusCode::IoError, "snapshot directory could not be created");
        }
    }
    std::filesystem::path temporary = path;
    temporary += ".tmp";

    std::FILE* file = std::fopen(temporary.string().c_str(), "wb");
    if (file == nullptr) {
        return make_status(StatusCode::IoError, "snapshot temporary file could not be created");
    }
    ByteWriter header(kSnapshotHeaderBytes);
    header.write_bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(kSnapshotMagic),
                                                  sizeof(kSnapshotMagic)));
    header.write_u32(kSchemaVersion);
    header.write_u64(static_cast<u64>(payload.size()));
    header.write_u32(crc32c(payload));

    bool ok = std::fwrite(header.buffer().data(), 1, header.buffer().size(), file) ==
              header.buffer().size();
    ok = ok && (payload.empty() ||
                std::fwrite(payload.data(), 1, payload.size(), file) == payload.size());
    Status status = ok ? flush_to_stable(file) : make_status(StatusCode::IoError,
                                                             "snapshot write failed");
    std::fclose(file);
    if (!status.ok()) {
        std::filesystem::remove(temporary, error);
        return status;
    }
    status = rename_over(temporary, path);
    if (!status.ok()) {
        std::filesystem::remove(temporary, error);
    }
    return status;
}

StatusOr<std::vector<std::byte>> SnapshotFile::read(const std::filesystem::path& path,
                                                     Options options) {
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return make_status(StatusCode::NotFound, "snapshot file could not be opened");
    }
    std::byte header[kSnapshotHeaderBytes];
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header)) {
        std::fclose(file);
        return make_status(StatusCode::Truncated, "snapshot header is truncated");
    }
    if (std::memcmp(header, kSnapshotMagic, sizeof(kSnapshotMagic)) != 0) {
        std::fclose(file);
        return make_status(StatusCode::Corrupt, "snapshot magic does not match");
    }
    u32 schema = 0;
    u64 length = 0;
    u32 stored_crc = 0;
    std::memcpy(&schema, header + 8, sizeof(schema));
    std::memcpy(&length, header + 12, sizeof(length));
    std::memcpy(&stored_crc, header + 8 + 4 + 8, sizeof(stored_crc));
    if (schema != kSchemaVersion) {
        std::fclose(file);
        return make_status(StatusCode::VersionMismatch, "snapshot schema is not supported");
    }
    if (length > options.max_bytes) {
        std::fclose(file);
        return make_status(StatusCode::Oversized, "snapshot length exceeds the ceiling");
    }
    std::vector<std::byte> payload(static_cast<std::size_t>(length));
    if (length != 0 && std::fread(payload.data(), 1, payload.size(), file) != payload.size()) {
        std::fclose(file);
        return make_status(StatusCode::Truncated, "snapshot payload is truncated");
    }
    std::fclose(file);
    if (crc32c(payload) != stored_crc) {
        return make_status(StatusCode::IntegrityFailure, "snapshot checksum does not match");
    }
    return payload;
}

// --- DurableGovernor --------------------------------------------------------

namespace {

std::filesystem::path journal_path(const DurableGovernor::Options& options) {
    std::filesystem::path path = options.directory;
    path /= "journal.efgj";
    return path;
}

std::filesystem::path snapshot_path(const DurableGovernor::Options& options) {
    std::filesystem::path path = options.directory;
    path /= "snapshot.efgs";
    return path;
}

void add_finding(RecoveryReport& report, RecoveryDisposition disposition, RecordType type,
                 u64 sequence, u64 count, std::string detail) {
    if (report.findings.size() >= 64) {
        return;
    }
    RecoveryFinding finding;
    finding.disposition = disposition;
    finding.type = type;
    finding.sequence = sequence;
    finding.count = count;
    finding.detail = std::move(detail);
    report.findings.push_back(std::move(finding));
}

}  // namespace

StatusOr<DurableGovernor> DurableGovernor::open(Options options, GovernorConfig governor_config,
                                                BootIdentity boot, Tick recovery_tick) {
    if (options.directory.empty()) {
        return make_status(StatusCode::InvalidArgument, "durable store directory is empty");
    }
    if (!boot.valid()) {
        return make_status(StatusCode::InvalidArgument, "durable store requires a boot identity");
    }
    std::error_code error;
    std::filesystem::create_directories(options.directory, error);
    if (error) {
        return make_status(StatusCode::IoError, "durable store directory could not be created");
    }

    DurableGovernor store;
    store.options_ = options;
    store.governor_ = std::make_unique<Governor>(governor_config);
    store.governor_->set_incarnation(boot);

    RecoveryReport& report = store.report_;
    report.recovery_tick = recovery_tick;
    report.current_boot = boot.boot;
    report.current_epoch = boot.epoch;

    // 1. Snapshot first: it carries the committed authoritative state.
    const std::filesystem::path snapshot = snapshot_path(options);
    if (std::filesystem::exists(snapshot, error) && !error) {
        StatusOr<std::vector<std::byte>> payload =
            SnapshotFile::read(snapshot, options.snapshot);
        if (payload.ok()) {
            // The stored incarnation is read first so that the recovery report
            // names the identity that produced the state being replaced.
            {
                ByteReader peek(payload.value());
                u32 schema = 0;
                u32 revision = 0;
                u64 last_tick = 0;
                u64 classification_sequence = 0;
                u64 intent_sequence = 0;
                u64 stored_boot = 0;
                u64 stored_epoch = 0;
                if (peek.read_u32(schema) && peek.read_u32(revision) && peek.read_u64(last_tick) &&
                    peek.read_u64(classification_sequence) && peek.read_u64(intent_sequence) &&
                    peek.read_u64(stored_boot) && peek.read_u64(stored_epoch)) {
                    report.previous_boot = BootId{stored_boot};
                    report.previous_epoch = EpochId{stored_epoch};
                }
            }
            ByteReader reader(payload.value());
            const Status restored = store.governor_->deserialize(reader, recovery_tick);
            if (restored.ok()) {
                report.snapshot_loaded = true;
                add_finding(report, RecoveryDisposition::CommittedState, RecordType::Snapshot, 0, 1,
                            "snapshot restored; all authority demoted to revalidation");
            } else if (restored.code() == StatusCode::Conflict ||
                       restored.code() == StatusCode::EpochMismatch ||
                       restored.code() == StatusCode::VersionMismatch) {
                // A snapshot that belongs to a different incarnation is not a
                // damaged artifact: it is a refusal, and it is final.
                return restored;
            } else {
                report.snapshot_rejected = true;
                add_finding(report, RecoveryDisposition::AmbiguousOutcome, RecordType::Snapshot, 0, 1,
                            std::string{"snapshot rejected: "} + std::string{restored.message()});
            }
        } else {
            report.snapshot_rejected = true;
            add_finding(report, RecoveryDisposition::AmbiguousOutcome, RecordType::Snapshot, 0, 1,
                        std::string{"snapshot unreadable: "} +
                            std::string{payload.status().message()});
        }
    }

    // 2. Journal replay: the authoritative input stream since the snapshot.
    StatusOr<Journal> journal = Journal::open(journal_path(options), options.journal);
    if (!journal.ok()) {
        return journal.status();
    }
    store.journal_ = std::move(journal.value());
    const Status replayed = store.replay_into_governor(recovery_tick);
    if (!replayed.ok()) {
        return replayed;
    }

    // 3. Everything that did not survive the restart is reported, not resurrected.
    const GovernorCounters& counters = store.governor_->counters();
    if (counters.restored_flows_requiring_revalidation > 0) {
        add_finding(report, RecoveryDisposition::EvidenceRequiresRevalidation, RecordType::FlowClassified,
                    0, counters.restored_flows_requiring_revalidation,
                    "restored classifications were elephants before the restart");
    }
    if (counters.intents_fenced > 0) {
        add_finding(report, RecoveryDisposition::StaleLiveAuthority, RecordType::IntentFenced, 0,
                    counters.intents_fenced, "live governance intent was fenced by the new boot");
    }

    std::span<const std::byte> begin_payload{};
    const Status begun = store.journal_.append(RecordType::Begin, begin_payload);
    if (!begun.ok()) {
        return begun;
    }
    store.open_ = true;
    return std::move(store);
}

Status DurableGovernor::replay_into_governor(Tick recovery_tick) {
    const std::filesystem::path path = journal_path(options_);
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return {};
    }

    JournalReplayStats stats;
    Status status = Journal::replay(
        path,
        [this, recovery_tick](RecordType type, std::span<const std::byte> payload,
                              u64 sequence) -> Status {
            report_.records_replayed += 1;
            report_.bytes_replayed += payload.size();
            switch (type) {
                case RecordType::Begin:
                    add_finding(report_, RecoveryDisposition::DurableConfiguration, type, sequence, 1,
                                "store incarnation marker");
                    return {};
                case RecordType::PolicyAccepted: {
                    ByteReader reader(payload);
                    PolicyDocument policy;
                    EFG_TRY(decode(reader, policy));
                    const Status applied = governor_->submit_policy(policy);
                    if (!applied.ok() && applied.code() != StatusCode::Stale &&
                        applied.code() != StatusCode::Conflict) {
                        return applied;
                    }
                    add_finding(report_, RecoveryDisposition::DurableConfiguration, type, sequence, 1,
                                "policy generation replayed");
                    return {};
                }
                case RecordType::CapacityAccepted: {
                    ByteReader reader(payload);
                    ResourceCapacity capacity;
                    EFG_TRY(decode(reader, capacity));
                    const Status applied = governor_->submit_capacity(capacity);
                    if (!applied.ok() && applied.code() != StatusCode::Stale &&
                        applied.code() != StatusCode::Conflict) {
                        return applied;
                    }
                    add_finding(report_, RecoveryDisposition::DurableConfiguration, type, sequence, 1,
                                "capacity snapshot replayed");
                    return {};
                }
                case RecordType::PathAccepted: {
                    ByteReader reader(payload);
                    PathDescriptor path_descriptor;
                    EFG_TRY(decode(reader, path_descriptor));
                    const Status applied = governor_->submit_path(path_descriptor);
                    if (!applied.ok() && applied.code() != StatusCode::Stale &&
                        applied.code() != StatusCode::Conflict) {
                        return applied;
                    }
                    add_finding(report_, RecoveryDisposition::DurableConfiguration, type, sequence, 1,
                                "path generation replayed");
                    return {};
                }
                case RecordType::FlowClassified: {
                    ByteReader reader(payload);
                    FlowSample sample;
                    EFG_TRY(decode(reader, sample));
                    const StatusOr<FlowDecision> applied = governor_->submit_evidence(sample);
                    if (!applied.ok() && applied.code() != StatusCode::Stale &&
                        applied.code() != StatusCode::NotFound) {
                        // Duplicates and contradictions are expected when a
                        // snapshot already folded this evidence in.
                        report_.records_rejected += 1;
                        add_finding(report_, RecoveryDisposition::AmbiguousOutcome, type, sequence, 1,
                                    std::string{"evidence not applied: "} +
                                        std::string{applied.status().message()});
                        return {};
                    }
                    return {};
                }
                case RecordType::FlowTransition: {
                    ByteReader reader(payload);
                    FlowId flow{};
                    Generation generation{};
                    u64 raw_flow = 0;
                    u64 raw_generation = 0;
                    u64 at = 0;
                    if (!reader.read_u64(raw_flow) || !reader.read_u64(raw_generation) ||
                        !reader.read_u64(at)) {
                        return make_status(StatusCode::Truncated, "completion record is truncated");
                    }
                    flow = FlowId{raw_flow};
                    generation = Generation{raw_generation};
                    const Status applied = governor_->complete_flow(flow, generation, at);
                    if (!applied.ok() && applied.code() != StatusCode::NotFound) {
                        return applied;
                    }
                    add_finding(report_, RecoveryDisposition::CommittedState, type, sequence, 1,
                                "flow completion replayed");
                    return {};
                }
                case RecordType::TickCommitted: {
                    ByteReader reader(payload);
                    u64 tick = 0;
                    if (!reader.read_u64(tick)) {
                        return make_status(StatusCode::Truncated, "tick record is truncated");
                    }
                    if (tick <= recovery_tick) {
                        (void)governor_->tick(tick);
                    }
                    add_finding(report_, RecoveryDisposition::CommittedState, type, sequence, 1,
                                "committed sweep replayed");
                    return {};
                }
                case RecordType::Snapshot:
                    add_finding(report_, RecoveryDisposition::DurableConfiguration, type, sequence, 1,
                                "compaction marker");
                    return {};
                case RecordType::Shutdown:
                    add_finding(report_, RecoveryDisposition::DurableConfiguration, type, sequence, 1,
                                "clean shutdown marker");
                    return {};
                case RecordType::IntentRecorded:
                case RecordType::IntentTransition:
                case RecordType::IntentFenced:
                case RecordType::Count:
                default:
                    add_finding(report_, RecoveryDisposition::DurableConfiguration, type, sequence, 1,
                                "record retained but not replayed");
                    return {};
            }
        },
        stats);
    if (!status.ok() && status.code() != StatusCode::IntegrityFailure) {
        return status;
    }
    report_.records_replayed = stats.records;
    report_.bytes_replayed = stats.bytes;
    report_.journal_truncated_tail = stats.truncated_tail;
    report_.journal_corrupt = !status.ok();
    if (stats.truncated_tail) {
        add_finding(report_, RecoveryDisposition::UnfinishedAttempt, RecordType::Begin,
                    stats.sequence_last, 1, "journal tail was torn; the partial record was not applied");
    }
    if (!status.ok()) {
        add_finding(report_, RecoveryDisposition::AmbiguousOutcome, RecordType::Begin,
                    stats.sequence_last, 1, "journal record failed integrity checking mid stream");
    }
    return {};
}

StatusOr<std::vector<std::byte>> DurableGovernor::encode_snapshot() const {
    ByteWriter writer(options_.snapshot.max_bytes);
    const Status serialized = governor_->serialize(writer);
    if (!serialized.ok()) {
        return serialized;
    }
    if (writer.overflowed()) {
        return make_status(StatusCode::Oversized, "snapshot serialization exceeded the ceiling");
    }
    return writer.take();
}

Status DurableGovernor::record_policy(const PolicyDocument& policy) {
    ByteWriter writer(limits::kMaxJournalRecordBytes);
    encode(writer, policy);
    if (writer.overflowed()) {
        return make_status(StatusCode::Oversized, "policy record exceeded the ceiling");
    }
    EFG_TRY(governor_->submit_policy(policy));
    EFG_TRY(journal_.append(RecordType::PolicyAccepted, writer.buffer()));
    ++records_since_snapshot_;
    return {};
}

Status DurableGovernor::record_capacity(const ResourceCapacity& capacity) {
    ByteWriter writer(limits::kMaxJournalRecordBytes);
    encode(writer, capacity);
    if (writer.overflowed()) {
        return make_status(StatusCode::Oversized, "capacity record exceeded the ceiling");
    }
    EFG_TRY(governor_->submit_capacity(capacity));
    EFG_TRY(journal_.append(RecordType::CapacityAccepted, writer.buffer()));
    ++records_since_snapshot_;
    return {};
}

Status DurableGovernor::record_path(const PathDescriptor& path) {
    ByteWriter writer(limits::kMaxJournalRecordBytes);
    encode(writer, path);
    if (writer.overflowed()) {
        return make_status(StatusCode::Oversized, "path record exceeded the ceiling");
    }
    EFG_TRY(governor_->submit_path(path));
    EFG_TRY(journal_.append(RecordType::PathAccepted, writer.buffer()));
    ++records_since_snapshot_;
    return {};
}

StatusOr<FlowDecision> DurableGovernor::record_evidence(const FlowSample& sample) {
    ByteWriter writer(limits::kMaxJournalRecordBytes);
    encode(writer, sample);
    if (writer.overflowed()) {
        return make_status(StatusCode::Oversized, "evidence record exceeded the ceiling");
    }
    // The evidence is made durable before it can influence any decision, so a
    // crash can never lose an input that already produced authority.
    EFG_TRY(journal_.append(RecordType::FlowClassified, writer.buffer()));
    ++records_since_snapshot_;
    StatusOr<FlowDecision> decision = governor_->submit_evidence(sample);
    if (options_.snapshot_every_records != 0 &&
        records_since_snapshot_ >= options_.snapshot_every_records) {
        (void)compact_snapshot();
    }
    return decision;
}

StatusOr<DecisionBatch> DurableGovernor::record_tick(Tick now) {
    ByteWriter writer(16);
    writer.write_u64(now);
    EFG_TRY(journal_.append(RecordType::TickCommitted, writer.buffer()));
    ++records_since_snapshot_;
    return governor_->tick(now);
}

Status DurableGovernor::compact_snapshot() {
    StatusOr<std::vector<std::byte>> payload = encode_snapshot();
    if (!payload.ok()) {
        return payload.status();
    }
    EFG_TRY(SnapshotFile::write_atomic(snapshot_path(options_), payload.value(), options_.snapshot));
    EFG_TRY(journal_.append(RecordType::Snapshot, {}));
    EFG_TRY(journal_.reset());
    records_since_snapshot_ = 0;
    return {};
}

Status DurableGovernor::close() {
    if (!open_) {
        return {};
    }
    (void)journal_.append(RecordType::Shutdown, {});
    EFG_TRY(journal_.close());
    open_ = false;
    return {};
}

}  // namespace efg
