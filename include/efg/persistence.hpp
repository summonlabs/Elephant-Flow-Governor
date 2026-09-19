// Elephant Flow Governor - durable state.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Durable mutation follows validate -> bind authority -> plan -> reserve ->
// journal -> perform -> verify -> commit -> retire. Nothing is acknowledged
// before the required state is durable.
//
// Every durable artifact is versioned and integrity checked. A record is
// rejected - never partially applied - when its magic, schema version, length or
// CRC-32C does not match. Recovery classifies what it finds into durable
// configuration and history, committed authoritative state, unfinished attempts,
// ambiguous outcomes, stale live authority and evidence that requires
// revalidation. Restoring never re-establishes liveness.

#ifndef EFG_PERSISTENCE_HPP
#define EFG_PERSISTENCE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "efg/checked.hpp"
#include "efg/governor.hpp"
#include "efg/hash.hpp"
#include "efg/limits.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"

namespace efg {

enum class RecordType : std::uint16_t {
    Begin = 1,
    PolicyAccepted = 2,
    CapacityAccepted = 3,
    PathAccepted = 4,
    FlowClassified = 5,
    FlowTransition = 6,
    IntentRecorded = 7,
    IntentTransition = 8,
    IntentFenced = 9,
    TickCommitted = 10,
    Snapshot = 11,
    Shutdown = 12,
    Count = 13,
};

[[nodiscard]] std::string_view to_string(RecordType type) noexcept;

/// Result of inspecting a durable artifact.
enum class RecoveryDisposition : std::uint8_t {
    /// Configuration and policy history. Durable, replayable, still meaningful.
    DurableConfiguration = 0,
    /// Committed authoritative state that survived integrity checking.
    CommittedState = 1,
    /// An attempt that began and never committed. Must not be resumed as if it
    /// had succeeded; it is reported and retired.
    UnfinishedAttempt = 2,
    /// A durable outcome that cannot be classified as success or failure.
    /// Reported as ambiguous and never treated as success.
    AmbiguousOutcome = 3,
    /// Live authority that did not survive the restart. Fenced on recovery.
    StaleLiveAuthority = 4,
    /// Evidence that must be revalidated before it can justify anything again.
    EvidenceRequiresRevalidation = 5,
};

[[nodiscard]] std::string_view to_string(RecoveryDisposition disposition) noexcept;

struct RecoveryFinding {
    RecoveryDisposition disposition{RecoveryDisposition::DurableConfiguration};
    RecordType type{RecordType::Begin};
    u64 sequence{0};
    u64 count{0};
    std::string detail{};
};

struct RecoveryReport {
    bool snapshot_loaded{false};
    bool snapshot_rejected{false};
    bool journal_truncated_tail{false};
    bool journal_corrupt{false};
    u64 records_replayed{0};
    u64 records_rejected{0};
    u64 bytes_replayed{0};
    Tick restored_tick{0};
    Tick recovery_tick{0};
    EpochId previous_epoch{};
    EpochId current_epoch{};
    BootId previous_boot{};
    BootId current_boot{};
    u64 flows_restored{0};
    u64 flows_requiring_revalidation{0};
    u64 intents_fenced{0};
    std::vector<RecoveryFinding> findings{};

    [[nodiscard]] Status validate() const;
};

struct JournalReplayStats {
    u64 records{0};
    u64 bytes{0};
    u64 sequence_last{0};
    u64 rejected{0};
    bool truncated_tail{false};
    bool header_invalid{false};
    bool oversize_record{false};
};

/// Append-only, CRC checked, length prefixed journal.
///
/// Layout of the file header: magic "EFGJRNL1" (8 bytes), schema version (u32),
/// header CRC-32C (u32). Layout of each record: magic 0xEF6A (u16), type (u16),
/// schema version (u32), payload length (u32), sequence (u64), payload bytes,
/// payload CRC-32C (u32). A record whose header or payload fails to verify
/// terminates replay: the tail is reported as truncated and never applied.
class Journal {
public:
    struct Options {
        std::size_t max_record_bytes{limits::kMaxJournalRecordBytes};
        u64 max_file_bytes{limits::kMaxJournalFileBytes};
        bool fsync_on_commit{true};
    };

    Journal() = default;
    ~Journal();

    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&& other) noexcept;
    Journal& operator=(Journal&& other) noexcept;

    /// Open for append. Creates the file (and its parent directory) when absent.
    /// An existing file with a damaged header is refused, not repaired.
    [[nodiscard]] static StatusOr<Journal> open(const std::filesystem::path& path,
                                                Options options = {});

    [[nodiscard]] bool is_open() const noexcept { return file_ != nullptr; }
    [[nodiscard]] u64 sequence() const noexcept { return sequence_; }
    [[nodiscard]] u64 size_bytes() const noexcept { return size_bytes_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    /// Append a record and optionally force it to stable storage before returning.
    Status append(RecordType type, std::span<const std::byte> payload);
    Status append(RecordType type, std::span<const std::byte> payload, bool durable);

    /// Force previously appended bytes to stable storage.
    Status sync();

    /// Rewrite the file down to a bare header and resume appending. Used by
    /// snapshot compaction once the snapshot itself is durable.
    Status reset();

    Status close();

    /// Replay every intact record in order, invoking the visitor. The visitor
    /// returns a Status; a failing visitor stops replay without tearing down the
    /// already-applied prefix.
    [[nodiscard]] static Status replay(const std::filesystem::path& path,
                                       const std::function<Status(RecordType, std::span<const std::byte>,
                                                                  u64 sequence)>& visitor,
                                       JournalReplayStats& stats);

    [[nodiscard]] static Status verify(const std::filesystem::path& path, JournalReplayStats& stats);

    /// Parse one record header from a buffer. Exposed for adversarial tests that
    /// need to mutate individual header fields.
    [[nodiscard]] static Status parse_header(std::span<const std::byte> bytes,
                                             std::size_t max_record_bytes,
                                             RecordType& type,
                                             u32& payload_length,
                                             u64& sequence,
                                             std::size_t& header_size);

    static constexpr std::size_t kFileHeaderSize = 16;
    static constexpr std::size_t kRecordHeaderSize = 20;

private:
    std::filesystem::path path_{};
    void* file_{nullptr};
    Options options_{};
    u64 sequence_{0};
    u64 size_bytes_{0};
};

/// Crash safe snapshot file: written to a sibling temporary, forced to stable
/// storage, then atomically renamed over the destination.
class SnapshotFile {
public:
    struct Options {
        std::size_t max_bytes{limits::kMaxSnapshotBytes};
        bool fsync_on_commit{true};
    };

    [[nodiscard]] static Status write_atomic(const std::filesystem::path& path,
                                             std::span<const std::byte> payload,
                                             Options options = {});

    [[nodiscard]] static StatusOr<std::vector<std::byte>> read(const std::filesystem::path& path,
                                                              Options options = {});
};

/// A governor plus the durable store that backs it.
///
/// Writes are journalled; the snapshot is a compaction artifact. Recovery reads
/// the snapshot, replays the journal after it, reports findings and then fences
/// everything that did not survive the restart.
class DurableGovernor {
public:
    struct Options {
        std::filesystem::path directory{};
        Journal::Options journal{};
        SnapshotFile::Options snapshot{};
        bool snapshot_on_commit{false};
        u64 snapshot_every_records{0};
    };

    DurableGovernor() = default;

    /// Open an existing store or create a new one, then recover the governor.
    ///
    /// The boot identity must be freshly minted for this process and its epoch
    /// must be beyond the epoch recorded in the snapshot; recovery refuses to
    /// resurrect authority under an identity that already held it.
    [[nodiscard]] static StatusOr<DurableGovernor> open(Options options,
                                                        GovernorConfig governor_config,
                                                        BootIdentity boot,
                                                        Tick recovery_tick);

    [[nodiscard]] Governor& governor() noexcept { return *governor_; }
    [[nodiscard]] const Governor& governor() const noexcept { return *governor_; }
    [[nodiscard]] const RecoveryReport& recovery() const noexcept { return report_; }
    [[nodiscard]] const Options& options() const noexcept { return options_; }

    /// Submit a policy and durably record the acceptance before returning.
    Status record_policy(const PolicyDocument& policy);
    Status record_capacity(const ResourceCapacity& capacity);
    Status record_path(const PathDescriptor& path);
    [[nodiscard]] StatusOr<FlowDecision> record_evidence(const FlowSample& sample);
    [[nodiscard]] StatusOr<DecisionBatch> record_tick(Tick now);

    Status compact_snapshot();
    Status close();

private:
    [[nodiscard]] Status replay_into_governor(Tick recovery_tick);
    [[nodiscard]] StatusOr<std::vector<std::byte>> encode_snapshot() const;

    Options options_{};
    std::unique_ptr<Governor> governor_{};
    Journal journal_{};
    RecoveryReport report_{};
    u64 records_since_snapshot_{0};
    bool open_{false};
};

}  // namespace efg

#endif  // EFG_PERSISTENCE_HPP
