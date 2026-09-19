// Elephant Flow Governor - durable state, restart and recovery tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The governing rule under test: durable state must not silently restore
// liveness, telemetry freshness, publisher authority or hardware effect.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace efg;
using namespace efgtest;

namespace {

class ScratchDirectory {
public:
    explicit ScratchDirectory(const std::string& name) {
        path_ = std::filesystem::temp_directory_path() /
                (name + "-" + unique_scratch_suffix());
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        EFG_CHECK(!error);
    }
    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::filesystem::path file(const std::string& name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

std::vector<std::byte> make_payload(std::size_t size, u8 seed) {
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>(static_cast<u8>((i * 31u + seed) & 0xFFu));
    }
    return out;
}

}  // namespace

EFG_TEST(persistence, a_journal_round_trips_records_in_order) {
    ScratchDirectory scratch("efg-journal-roundtrip");
    const std::filesystem::path path = scratch.file("journal.efgj");
    {
        StatusOr<Journal> journal = Journal::open(path);
        EFG_REQUIRE(journal.ok());
        for (u8 i = 0; i < 8; ++i) {
            const std::vector<std::byte> payload = make_payload(16 + i, i);
            EFG_CHECK_STATUS_OK(journal.value().append(RecordType::FlowClassified, payload));
        }
        EFG_CHECK_EQ(journal.value().sequence(), 8u);
        EFG_CHECK_STATUS_OK(journal.value().close());
    }

    std::vector<u64> sequences;
    std::size_t total = 0;
    JournalReplayStats stats;
    EFG_CHECK_STATUS_OK(Journal::replay(
        path,
        [&sequences, &total](RecordType type, std::span<const std::byte> payload, u64 sequence) {
            EFG_CHECK_EQ(type, RecordType::FlowClassified);
            EFG_CHECK(!payload.empty());
            sequences.push_back(sequence);
            total += payload.size();
            return Status{};
        },
        stats));
    EFG_CHECK_EQ(sequences.size(), 8u);
    EFG_CHECK_EQ(total, 8 * 16 + 28);
    for (std::size_t i = 0; i < sequences.size(); ++i) {
        EFG_CHECK_EQ(sequences[i], i + 1);
    }
    EFG_CHECK(!stats.truncated_tail);
    EFG_CHECK_EQ(stats.rejected, 0u);

    // Reopening an intact journal resumes the sequence rather than restarting it.
    StatusOr<Journal> reopened = Journal::open(path);
    EFG_REQUIRE(reopened.ok());
    EFG_CHECK_EQ(reopened.value().sequence(), 8u);
    EFG_CHECK_STATUS_OK(reopened.value().close());
}

EFG_TEST(persistence, a_torn_tail_is_reported_and_never_applied) {
    ScratchDirectory scratch("efg-journal-torn");
    const std::filesystem::path path = scratch.file("journal.efgj");
    {
        StatusOr<Journal> journal = Journal::open(path);
        EFG_REQUIRE(journal.ok());
        for (u8 i = 0; i < 3; ++i) {
            EFG_CHECK_STATUS_OK(
                journal.value().append(RecordType::FlowClassified, make_payload(32, i)));
        }
        EFG_CHECK_STATUS_OK(journal.value().close());
    }
    // Simulate a crash in the middle of writing a fourth record.
    {
        std::FILE* file = std::fopen(path.string().c_str(), "ab");
        EFG_REQUIRE(file != nullptr);
        const std::byte partial[9] = {};
        EFG_CHECK_EQ(std::fwrite(partial, 1, sizeof(partial), file), sizeof(partial));
        std::fclose(file);
    }

    std::size_t applied = 0;
    JournalReplayStats stats;
    EFG_CHECK_STATUS_OK(Journal::replay(
        path,
        [&applied](RecordType, std::span<const std::byte>, u64) {
            ++applied;
            return Status{};
        },
        stats));
    EFG_CHECK_EQ(applied, 3u);
    EFG_CHECK(stats.truncated_tail);
}

EFG_TEST(persistence, a_record_damaged_in_the_middle_is_refused_not_skipped) {
    ScratchDirectory scratch("efg-journal-damaged");
    const std::filesystem::path path = scratch.file("journal.efgj");
    {
        StatusOr<Journal> journal = Journal::open(path);
        EFG_REQUIRE(journal.ok());
        for (u8 i = 0; i < 4; ++i) {
            EFG_CHECK_STATUS_OK(
                journal.value().append(RecordType::FlowClassified, make_payload(64, i)));
        }
        EFG_CHECK_STATUS_OK(journal.value().close());
    }
    {
        std::FILE* file = std::fopen(path.string().c_str(), "r+b");
        EFG_REQUIRE(file != nullptr);
        // Damage a byte inside the second record's payload.
        EFG_CHECK_EQ(std::fseek(file, 16 + 20 + 64 + 20 + 10, SEEK_SET), 0);
        std::fputc('!', file);
        std::fclose(file);
    }

    std::size_t applied = 0;
    JournalReplayStats stats;
    const Status status = Journal::replay(
        path,
        [&applied](RecordType, std::span<const std::byte>, u64) {
            ++applied;
            return Status{};
        },
        stats);
    EFG_CHECK_EQ(status.code(), StatusCode::IntegrityFailure);
    EFG_CHECK_EQ(applied, 1u);
    EFG_CHECK_EQ(stats.rejected, 1u);
}

EFG_TEST(persistence, a_record_that_declares_an_absurd_length_is_refused) {
    ScratchDirectory scratch("efg-journal-oversize");
    const std::filesystem::path path = scratch.file("journal.efgj");
    {
        StatusOr<Journal> journal = Journal::open(path);
        EFG_REQUIRE(journal.ok());
        EFG_CHECK_STATUS_OK(journal.value().append(RecordType::FlowClassified, make_payload(8, 1)));
        EFG_CHECK_STATUS_OK(journal.value().close());
    }
    {
        std::FILE* file = std::fopen(path.string().c_str(), "r+b");
        EFG_REQUIRE(file != nullptr);
        EFG_CHECK_EQ(std::fseek(file, 16 + 8, SEEK_SET), 0);  // the length field
        const u32 absurd = 0xF0000000u;
        EFG_CHECK_EQ(std::fwrite(&absurd, 1, sizeof(absurd), file), sizeof(absurd));
        std::fclose(file);
    }
    JournalReplayStats stats;
    EFG_CHECK_EQ(Journal::verify(path, stats).code(), StatusCode::Oversized);
    EFG_CHECK(stats.oversize_record);
}

EFG_TEST(persistence, a_journal_refuses_to_grow_past_its_ceiling) {
    ScratchDirectory scratch("efg-journal-ceiling");
    const std::filesystem::path path = scratch.file("journal.efgj");
    Journal::Options options;
    options.max_file_bytes = 256;
    StatusOr<Journal> journal = Journal::open(path, options);
    EFG_REQUIRE(journal.ok());
    bool refused = false;
    for (int i = 0; i < 64; ++i) {
        const Status status =
            journal.value().append(RecordType::FlowClassified, make_payload(32, static_cast<u8>(i)));
        if (!status.ok()) {
            EFG_CHECK_EQ(status.code(), StatusCode::Oversized);
            refused = true;
            break;
        }
    }
    EFG_CHECK(refused);
    EFG_CHECK_STATUS_OK(journal.value().close());
}

EFG_TEST(persistence, a_snapshot_is_written_atomically_and_verified) {
    ScratchDirectory scratch("efg-snapshot-atomic");
    const std::filesystem::path path = scratch.file("snapshot.efgs");
    const std::vector<std::byte> payload = make_payload(4096, 7);
    EFG_CHECK_STATUS_OK(SnapshotFile::write_atomic(path, payload));
    // No temporary artifact is left behind.
    EFG_CHECK(!std::filesystem::exists(scratch.file("snapshot.efgs.tmp")));

    StatusOr<std::vector<std::byte>> read = SnapshotFile::read(path);
    EFG_REQUIRE(read.ok());
    EFG_CHECK(read.value() == payload);

    SnapshotFile::Options small;
    small.max_bytes = 16;
    EFG_CHECK_EQ(SnapshotFile::write_atomic(path, payload, small).code(), StatusCode::Oversized);
    EFG_CHECK_EQ(SnapshotFile::read(path, small).status().code(), StatusCode::Oversized);
}

EFG_TEST(persistence, a_restart_demotes_every_classification_and_fences_every_intent) {
    ScratchDirectory scratch("efg-restart");
    DurableGovernor::Options options;
    options.directory = scratch.path();
    options.snapshot_on_commit = false;

    const Tick start = kStart;
    {
        StatusOr<DurableGovernor> store =
            DurableGovernor::open(options, GovernorConfig{}, boot(1, 1), start);
        EFG_REQUIRE(store.ok());
        DurableGovernor& durable = store.value();

        EFG_CHECK_STATUS_OK(durable.record_policy(default_policy(start, 1u << 20)));
        EFG_CHECK_STATUS_OK(durable.record_capacity(
            capacity(ResourceId{1}, 10000000, 1000000, start, 1u << 20)));
        EFG_CHECK_STATUS_OK(durable.record_path(path(PathId{1}, {ResourceId{1}}, start, 1u << 20)));

        for (int i = 0; i < 4; ++i) {
            EFG_CHECK_OK(durable.record_evidence(
                sample(FlowId{1}, EvidenceWindowId{static_cast<u64>(i) + 1}, start + i * 10,
                       start + (i + 1) * 10, 90000 * (static_cast<u64>(i) + 1), 90000, PathId{1},
                       Generation::initial(), static_cast<u64>(i) + 1)));
        }
        StatusOr<FlowClassification> classification = durable.governor().classification(
            FlowId{1}, Generation::initial(), start + 40);
        EFG_REQUIRE(classification.ok());
        EFG_CHECK_EQ(classification.value().state, ElephantState::Elephant);
        EFG_CHECK(durable.governor().intents().live_count() > 0);

        EFG_CHECK_STATUS_OK(durable.compact_snapshot());
        EFG_CHECK_STATUS_OK(durable.close());
    }

    // Restart with a fresh boot incarnation and an advanced epoch.
    StatusOr<DurableGovernor> reopened =
        DurableGovernor::open(options, GovernorConfig{}, boot(2, 2), start + 100);
    EFG_REQUIRE(reopened.ok());
    const RecoveryReport& report = reopened.value().recovery();
    EFG_CHECK(report.snapshot_loaded);
    EFG_CHECK(!report.snapshot_rejected);
    EFG_CHECK_EQ(report.previous_epoch.value(), 1u);
    EFG_CHECK_EQ(report.current_epoch.value(), 2u);
    EFG_CHECK(!report.findings.empty());
    EFG_CHECK_STATUS_OK(report.validate());

    bool saw_revalidation = false;
    bool saw_fenced = false;
    for (const RecoveryFinding& finding : report.findings) {
        if (finding.disposition == RecoveryDisposition::EvidenceRequiresRevalidation) {
            saw_revalidation = true;
        }
        if (finding.disposition == RecoveryDisposition::StaleLiveAuthority) {
            saw_fenced = true;
        }
    }
    EFG_CHECK(saw_revalidation);
    EFG_CHECK(saw_fenced);

    Governor& governor = reopened.value().governor();
    EFG_CHECK_EQ(governor.intents().live_count(), 0u);
    StatusOr<FlowClassification> restored =
        governor.classification(FlowId{1}, Generation::initial(), start + 100);
    EFG_REQUIRE(restored.ok());
    EFG_CHECK_EQ(restored.value().state, ElephantState::Suspended);
    EFG_CHECK_EQ(restored.value().authority, AuthorityLevel::None);
    EFG_CHECK(governor.counters().restored_flows >= 1);
    EFG_CHECK(governor.counters().restored_flows_requiring_revalidation >= 1);

    // Revalidation is genuinely required: fresh evidence re-earns the authority.
    EFG_CHECK_OK(reopened.value().record_evidence(
        sample(FlowId{1}, EvidenceWindowId{100}, start + 100, start + 110, 1000000, 90000, PathId{1},
               Generation::initial(), 100)));
    EFG_CHECK_OK(reopened.value().record_evidence(
        sample(FlowId{1}, EvidenceWindowId{101}, start + 110, start + 120, 1090000, 90000, PathId{1},
               Generation::initial(), 101)));
    StatusOr<FlowClassification> requalified =
        governor.classification(FlowId{1}, Generation::initial(), start + 120);
    EFG_REQUIRE(requalified.ok());
    EFG_CHECK_EQ(requalified.value().state, ElephantState::Elephant);
    EFG_CHECK_STATUS_OK(reopened.value().close());
}

EFG_TEST(persistence, restoring_under_the_stored_boot_identity_is_refused) {
    ScratchDirectory scratch("efg-restart-identity");
    DurableGovernor::Options options;
    options.directory = scratch.path();
    {
        StatusOr<DurableGovernor> store =
            DurableGovernor::open(options, GovernorConfig{}, boot(1, 1), kStart);
        EFG_REQUIRE(store.ok());
        EFG_CHECK_STATUS_OK(store.value().record_policy(default_policy(kStart, 1u << 20)));
        EFG_CHECK_STATUS_OK(store.value().compact_snapshot());
        EFG_CHECK_STATUS_OK(store.value().close());
    }
    // Same boot identifier: the runtime refuses to resurrect authority under an
    // identity that already held it.
    StatusOr<DurableGovernor> same_boot =
        DurableGovernor::open(options, GovernorConfig{}, boot(1, 2), kStart + 10);
    EFG_CHECK_EQ(same_boot.status().code(), StatusCode::Conflict);

    // Epoch that did not advance.
    StatusOr<DurableGovernor> same_epoch =
        DurableGovernor::open(options, GovernorConfig{}, boot(2, 1), kStart + 10);
    EFG_CHECK_EQ(same_epoch.status().code(), StatusCode::EpochMismatch);

    // A genuinely fresh incarnation succeeds.
    StatusOr<DurableGovernor> fresh =
        DurableGovernor::open(options, GovernorConfig{}, boot(2, 2), kStart + 10);
    EFG_REQUIRE(fresh.ok());
    EFG_CHECK_STATUS_OK(fresh.value().close());
}

EFG_TEST(persistence, an_absent_boot_identity_cannot_open_a_store) {
    ScratchDirectory scratch("efg-boot-required");
    DurableGovernor::Options options;
    options.directory = scratch.path();
    StatusOr<DurableGovernor> store =
        DurableGovernor::open(options, GovernorConfig{}, BootIdentity{}, kStart);
    EFG_CHECK_EQ(store.status().code(), StatusCode::InvalidArgument);

    DurableGovernor::Options empty;
    StatusOr<DurableGovernor> no_directory =
        DurableGovernor::open(empty, GovernorConfig{}, boot(1, 1), kStart);
    EFG_CHECK_EQ(no_directory.status().code(), StatusCode::InvalidArgument);
}

EFG_TEST(persistence, a_damaged_snapshot_is_reported_as_lost_state) {
    ScratchDirectory scratch("efg-damaged-snapshot");
    DurableGovernor::Options options;
    options.directory = scratch.path();
    {
        StatusOr<DurableGovernor> store =
            DurableGovernor::open(options, GovernorConfig{}, boot(1, 1), kStart);
        EFG_REQUIRE(store.ok());
        EFG_CHECK_STATUS_OK(store.value().record_policy(default_policy(kStart, 1u << 20)));
        EFG_CHECK_STATUS_OK(store.value().record_capacity(
            capacity(ResourceId{1}, 10000000, 1000000, kStart, 1u << 20)));
        EFG_CHECK_STATUS_OK(
            store.value().record_path(path(PathId{1}, {ResourceId{1}}, kStart, 1u << 20)));
        EFG_CHECK_OK(store.value().record_evidence(sample(FlowId{1}, EvidenceWindowId{1}, kStart,
                                                          kStart + 10, 1000, 1000, PathId{1},
                                                          Generation::initial(), 1)));
        EFG_CHECK_STATUS_OK(store.value().compact_snapshot());
        EFG_CHECK_STATUS_OK(store.value().close());
    }
    {
        std::FILE* file = std::fopen(scratch.file("snapshot.efgs").string().c_str(), "r+b");
        EFG_REQUIRE(file != nullptr);
        EFG_CHECK_EQ(std::fseek(file, 12, SEEK_SET), 0);
        std::fputc(0x01, file);
        std::fclose(file);
    }

    StatusOr<DurableGovernor> reopened =
        DurableGovernor::open(options, GovernorConfig{}, boot(5, 5), kStart + 50);
    EFG_REQUIRE(reopened.ok());
    EFG_CHECK(reopened.value().recovery().snapshot_rejected);
    bool saw_ambiguous = false;
    for (const RecoveryFinding& finding : reopened.value().recovery().findings) {
        if (finding.disposition == RecoveryDisposition::AmbiguousOutcome) {
            saw_ambiguous = true;
        }
    }
    EFG_CHECK(saw_ambiguous);
    // Compaction retires the journal once the snapshot is durable, so a damaged
    // snapshot means the folded history is genuinely gone. Recovery reports the
    // loss and reconstructs nothing, rather than fabricating authoritative state.
    EFG_CHECK_EQ(reopened.value().governor().flow_count(), 0u);
    EFG_CHECK_EQ(reopened.value().governor().counters().evidence_accepted, 0u);
    EFG_CHECK_STATUS_OK(reopened.value().close());
}

EFG_TEST(persistence, a_governor_cannot_be_restored_into_its_own_incarnation) {
    Governor governor;
    governor.set_incarnation(boot(1, 1));
    install_standard(governor);
    WindowFeeder feeder(governor, FlowId{1}, 10);
    qualify(feeder);

    ByteWriter writer(1u << 20);
    EFG_CHECK_STATUS_OK(governor.serialize(writer));
    EFG_CHECK(!writer.overflowed());

    Governor restored;
    restored.set_incarnation(boot(1, 1));  // same incarnation: refused
    ByteReader same(writer.buffer());
    EFG_CHECK_EQ(restored.deserialize(same, kStart + 200).code(), StatusCode::Conflict);

    Governor fresh;
    {
        ByteReader no_incarnation(writer.buffer());
        EFG_CHECK_EQ(fresh.deserialize(no_incarnation, kStart + 200).code(),
                     StatusCode::InvalidArgument);
    }

    fresh.set_incarnation(boot(2, 3));
    ByteReader reader(writer.buffer());
    EFG_CHECK_STATUS_OK(fresh.deserialize(reader, kStart + 200));
    EFG_CHECK_EQ(fresh.intents().live_count(), 0u);
    StatusOr<FlowClassification> classification =
        fresh.classification(FlowId{1}, Generation::initial(), kStart + 200);
    EFG_REQUIRE(classification.ok());
    EFG_CHECK_EQ(classification.value().state, ElephantState::Suspended);
}

EFG_TEST(persistence, serialization_rejects_a_truncated_stream) {
    Governor governor;
    governor.set_incarnation(boot(1, 1));
    install_standard(governor);
    WindowFeeder feeder(governor, FlowId{1}, 10);
    qualify(feeder);

    ByteWriter writer(1u << 20);
    EFG_CHECK_STATUS_OK(governor.serialize(writer));
    for (std::size_t cut : {0u, 8u, 32u, 64u, 96u}) {
        Governor restored;
        restored.set_incarnation(boot(9, 9));
        ByteReader reader(std::span<const std::byte>(writer.buffer().data(), cut));
        EFG_CHECK(!restored.deserialize(reader, kStart + 200).ok());
    }
}

EFG_TEST(persistence, the_journal_ceiling_refuses_rather_than_growing_without_bound) {
    ScratchDirectory scratch("efg-journal-bound");
    const std::filesystem::path path = scratch.file("journal.efgj");
    Journal::Options options;
    options.max_record_bytes = 128;
    StatusOr<Journal> journal = Journal::open(path, options);
    EFG_REQUIRE(journal.ok());
    EFG_CHECK_EQ(
        journal.value().append(RecordType::FlowClassified, make_payload(1024, 1)).code(),
        StatusCode::Oversized);
    EFG_CHECK_STATUS_OK(journal.value().close());
}
