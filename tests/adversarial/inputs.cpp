// Elephant Flow Governor - adversarial input tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every externally reachable decoder is driven with malformed, truncated,
// corrupt, oversized and contradictory input. Nothing here may crash, and
// nothing here may be silently accepted.

#include <cstdio>
#include <string>
#include <vector>

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace efg;
using namespace efgtest;

namespace {

std::vector<std::byte> encode_frame(const Frame& frame) {
    FrameCodec codec;
    ByteWriter writer(limits::kMaxFrameBytes);
    const Status status = codec.encode(frame, writer);
    EFG_CHECK_STATUS_OK(status);
    return writer.take();
}

Frame make_frame(FrameType type, std::span<const std::byte> payload) {
    Frame frame;
    frame.header.type = type;
    frame.header.epoch = EpochId{1};
    frame.header.boot = BootId{1};
    frame.header.worker = WorkerId{1};
    frame.header.sequence = 1;
    frame.payload.assign(payload.begin(), payload.end());
    return frame;
}

}  // namespace

EFG_TEST(framing, a_well_formed_frame_round_trips) {
    const std::byte payload[4] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const Frame frame = make_frame(FrameType::EvidenceBatch, std::span<const std::byte>(payload, 4));
    const std::vector<std::byte> bytes = encode_frame(frame);
    EFG_CHECK_EQ(bytes.size(), kFrameHeaderBytes + 4u);

    FrameCodec codec;
    Frame decoded;
    EFG_CHECK_STATUS_OK(codec.decode(bytes, decoded));
    EFG_CHECK_EQ(decoded.header.type, FrameType::EvidenceBatch);
    EFG_CHECK_EQ(decoded.header.sequence, 1u);
    EFG_CHECK_EQ(decoded.header.epoch.value(), 1u);
    EFG_CHECK_EQ(decoded.payload.size(), 4u);
}

EFG_TEST(framing, every_single_byte_corruption_is_detected) {
    const std::byte payload[8] = {std::byte{9}, std::byte{8}, std::byte{7}, std::byte{6},
                                  std::byte{5}, std::byte{4}, std::byte{3}, std::byte{2}};
    const Frame frame = make_frame(FrameType::Hello, std::span<const std::byte>(payload, 8));
    const std::vector<std::byte> original = encode_frame(frame);

    FrameCodec codec;
    for (std::size_t index = 0; index < original.size(); ++index) {
        for (int bit = 0; bit < 8; ++bit) {
            std::vector<std::byte> mutated = original;
            mutated[index] ^= static_cast<std::byte>(1u << bit);
            Frame decoded;
            const Status status = codec.decode(mutated, decoded);
            if (status.ok()) {
                // A flipped bit inside the payload can still pass the checksum
                // only if it is also reflected in the checksum, which it is not.
                EFG_FAIL(std::string{"corrupted frame decoded successfully at byte "} +
                         std::to_string(index) + " bit " + std::to_string(bit));
            }
        }
    }
}

EFG_TEST(framing, truncated_frames_never_decode) {
    const std::byte payload[16] = {};
    const Frame frame = make_frame(FrameType::EvidenceBatch, std::span<const std::byte>(payload, 16));
    const std::vector<std::byte> original = encode_frame(frame);
    FrameCodec codec;
    for (std::size_t cut = 0; cut < original.size(); ++cut) {
        Frame decoded;
        const Status status =
            codec.decode(std::span<const std::byte>(original.data(), cut), decoded);
        EFG_CHECK(!status.ok());
    }
}

EFG_TEST(framing, an_oversized_payload_length_is_refused_before_allocation) {
    FrameCodec codec{FrameCodec::Limits{1024}};
    std::vector<std::byte> bytes(kFrameHeaderBytes, std::byte{0});
    ByteWriter writer(kFrameHeaderBytes);
    writer.write_u32(kFrameMagic);
    writer.write_u32(kSchemaVersion);
    writer.write_u16(static_cast<u16>(FrameType::EvidenceBatch));
    writer.write_u16(0);
    writer.write_u64(1);
    writer.write_u64(1);
    writer.write_u64(1);
    writer.write_u64(1);
    writer.write_u32(0x00FFFFFFu);  // absurd declared length
    writer.write_u32(0);
    writer.write_u32(crc32c(std::span<const std::byte>(writer.buffer().data(), kFrameCrcOffset)));
    bytes = writer.take();

    Frame decoded;
    EFG_CHECK_EQ(codec.decode_header(bytes, decoded.header).code(), StatusCode::Oversized);
}

EFG_TEST(framing, a_foreign_magic_or_schema_is_refused) {
    const std::byte payload[2] = {};
    const Frame frame = make_frame(FrameType::Heartbeat, std::span<const std::byte>(payload, 2));
    std::vector<std::byte> bytes = encode_frame(frame);

    std::vector<std::byte> wrong_magic = bytes;
    wrong_magic[0] = std::byte{0};
    FrameCodec codec;
    Frame decoded;
    EFG_CHECK(!codec.decode(wrong_magic, decoded).ok());

    std::vector<std::byte> wrong_type = bytes;
    wrong_type[8] = std::byte{0x7F};
    wrong_type[9] = std::byte{0x7F};
    EFG_CHECK(!codec.decode(wrong_type, decoded).ok());
}

EFG_TEST(framing, an_unknown_type_is_not_encodable) {
    FrameCodec codec;
    ByteWriter writer(256);
    Frame frame;
    frame.header.type = FrameType::Invalid;
    EFG_CHECK_EQ(codec.encode(frame, writer).code(), StatusCode::InvalidArgument);
    frame.header.type = FrameType::Count;
    EFG_CHECK_EQ(codec.encode(frame, writer).code(), StatusCode::InvalidArgument);
}

EFG_TEST(framing, connecting_to_a_closed_port_fails_cleanly) {
    // Port 1 on loopback is not bound by this process; the connect must fail
    // rather than hang or dereference an invalid handle.
    const StatusOr<TcpStream> stream = connect_loopback(1);
    EFG_CHECK(!stream.ok());
}

EFG_TEST(framing, a_listener_reports_the_port_it_actually_bound) {
    StatusOr<TcpListener> listener = TcpListener::bind_loopback(0);
    EFG_REQUIRE(listener.ok());
    EFG_CHECK(listener.value().bound_port() != 0);
    StatusOr<TcpListener> second = TcpListener::bind_loopback(0);
    EFG_REQUIRE(second.ok());
    EFG_CHECK(second.value().bound_port() != listener.value().bound_port());
}

EFG_TEST(hostile, a_cyclic_rule_graph_is_refused) {
    RuleSet rules;
    RuleNode a;
    a.kind = RuleKind::Not;
    a.children = {1};
    RuleNode b;
    b.kind = RuleKind::Not;
    b.children = {0};
    rules.nodes = {a, b};
    rules.root = 0;
    EFG_CHECK_EQ(rules.validate().code(), StatusCode::InvalidArgument);
}

EFG_TEST(hostile, an_out_of_range_root_or_child_is_refused) {
    RuleSet rules;
    rules.nodes.push_back(RuleNode{RuleKind::Threshold,
                                   Threshold{ThresholdKind::Always, 0, ServiceClass::Unknown}, {}});
    rules.root = 5;
    EFG_CHECK_EQ(rules.validate().code(), StatusCode::InvalidArgument);

    RuleSet dangling;
    RuleNode root;
    root.kind = RuleKind::All;
    root.children = {7};
    dangling.nodes.push_back(root);
    dangling.root = 0;
    EFG_CHECK_EQ(dangling.validate().code(), StatusCode::InvalidArgument);
}

EFG_TEST(hostile, an_empty_or_mis_shaped_rule_set_is_refused) {
    RuleSet empty;
    EFG_CHECK_EQ(empty.validate().code(), StatusCode::InvalidArgument);
    EFG_CHECK_EQ(empty.evaluate(FlowMeasurements{}), Tri::Unknown);

    RuleSet leaf_with_children;
    leaf_with_children.nodes.push_back(
        RuleNode{RuleKind::Threshold, Threshold{ThresholdKind::Always, 0, ServiceClass::Unknown}, {1}});
    leaf_with_children.nodes.push_back(
        RuleNode{RuleKind::Threshold, Threshold{ThresholdKind::Always, 0, ServiceClass::Unknown}, {}});
    leaf_with_children.root = 0;
    EFG_CHECK_EQ(leaf_with_children.validate().code(), StatusCode::InvalidArgument);

    RuleSet not_with_two;
    RuleNode negate;
    negate.kind = RuleKind::Not;
    negate.children = {1, 2};
    not_with_two.nodes.push_back(negate);
    not_with_two.nodes.push_back(
        RuleNode{RuleKind::Threshold, Threshold{ThresholdKind::Always, 0, ServiceClass::Unknown}, {}});
    not_with_two.nodes.push_back(
        RuleNode{RuleKind::Threshold, Threshold{ThresholdKind::Never, 0, ServiceClass::Unknown}, {}});
    not_with_two.root = 0;
    EFG_CHECK_EQ(not_with_two.validate().code(), StatusCode::InvalidArgument);
}

EFG_TEST(hostile, an_excessively_large_rule_set_is_refused) {
    RuleSet rules;
    for (std::size_t i = 0; i <= limits::kMaxPolicyNodes; ++i) {
        rules.nodes.push_back(RuleNode{
            RuleKind::Threshold, Threshold{ThresholdKind::Always, 0, ServiceClass::Unknown}, {}});
    }
    rules.root = 0;
    EFG_CHECK_EQ(rules.validate().code(), StatusCode::Oversized);
}

EFG_TEST(hostile, a_policy_with_invalid_hysteresis_or_bounds_is_refused) {
    PolicyDocument policy = default_policy();
    policy.hysteresis.enter_confirm_windows = 0;
    EFG_CHECK_EQ(policy.validate().code(), StatusCode::InvalidArgument);

    PolicyDocument wide = default_policy();
    wide.authorization.max_shaping_reduction_bp = 20000;
    EFG_CHECK_EQ(wide.validate().code(), StatusCode::OutOfRange);

    PolicyDocument zero_lifetime = default_policy();
    zero_lifetime.authorization.max_intent_duration_ticks = 0;
    EFG_CHECK_EQ(zero_lifetime.validate().code(), StatusCode::InvalidArgument);

    PolicyDocument unfinalized = default_policy();
    unfinalized.digest = 0;
    EFG_CHECK_EQ(unfinalized.validate().code(), StatusCode::InvalidArgument);
}

EFG_TEST(hostile, a_policy_is_refused_when_its_validity_is_malformed) {
    PolicyDocument policy = default_policy();
    policy.validity = ValidityWindow{100, 100};
    EFG_CHECK_EQ(policy.validate().code(), StatusCode::InvalidArgument);

    Governor governor;
    governor.set_incarnation(boot());
    EFG_CHECK_EQ(governor.submit_policy(policy).code(), StatusCode::InvalidArgument);
}

EFG_TEST(hostile, a_same_generation_policy_with_different_content_is_a_conflict) {
    Governor governor;
    governor.set_incarnation(boot());
    EFG_CHECK_STATUS_OK(governor.submit_policy(default_policy()));
    PolicyDocument different = default_policy(kStart, 100000, 5000, 100000, 8, 300);
    EFG_CHECK_EQ(governor.submit_policy(different).code(), StatusCode::Conflict);
    EFG_CHECK(governor.counters().policy_rejected >= 1);
}

EFG_TEST(hostile, an_older_policy_generation_is_refused) {
    Governor governor;
    governor.set_incarnation(boot());
    PolicyDocument newer = default_policy();
    newer.generation = Generation{4};
    EFG_CHECK_STATUS_OK(newer.finalize());
    EFG_CHECK_STATUS_OK(governor.submit_policy(newer));

    PolicyDocument older = default_policy();
    older.generation = Generation{2};
    EFG_CHECK_STATUS_OK(older.finalize());
    EFG_CHECK_EQ(governor.submit_policy(older).code(), StatusCode::Stale);
}

EFG_TEST(hostile, a_capacity_snapshot_that_rolls_back_is_refused) {
    Governor governor;
    governor.set_incarnation(boot());
    EFG_CHECK_STATUS_OK(
        governor.submit_capacity(capacity(ResourceId{1}, 1000, 10, kStart, 100000, Generation{2}, 2)));
    EFG_CHECK_EQ(governor
                     .submit_capacity(
                         capacity(ResourceId{1}, 1000, 10, kStart, 100000, Generation{1}, 1))
                     .code(),
                 StatusCode::Stale);
    EFG_CHECK(governor.counters().capacity_rejected >= 1);
}

EFG_TEST(hostile, a_path_that_binds_a_resource_twice_is_refused) {
    Governor governor;
    governor.set_incarnation(boot());
    EFG_CHECK_EQ(
        governor.submit_path(path(PathId{1}, {ResourceId{1}, ResourceId{1}})).code(),
        StatusCode::ContradictoryEvidence);
    EFG_CHECK(governor.counters().path_rejected >= 1);
}

EFG_TEST(hostile, a_governor_with_an_invalid_configuration_reports_it) {
    GovernorConfig bad;
    bad.samples_per_flow = 1;
    EFG_CHECK_EQ(bad.validate().code(), StatusCode::OutOfRange);

    GovernorConfig huge;
    huge.max_flows = limits::kMaxFlows + 1;
    EFG_CHECK_EQ(huge.validate().code(), StatusCode::OutOfRange);

    GovernorConfig zero_validity;
    zero_validity.classification_validity_ticks = 0;
    EFG_CHECK_EQ(zero_validity.validate().code(), StatusCode::InvalidArgument);

    // The constructor clamps rather than throwing, and never exceeds a ceiling.
    Governor governor(huge);
    EFG_CHECK(governor.config().max_flows <= limits::kMaxFlows);
    EFG_CHECK(governor.config().samples_per_flow >= 2);
}

EFG_TEST(hostile, contractory_evidence_never_advances_the_ledger) {
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);

    EFG_CHECK_OK(governor.submit_evidence(
        sample(FlowId{1}, EvidenceWindowId{1}, kStart, kStart + 10, 1000, 1000, PathId{1},
               Generation::initial(), 1)));
    EFG_CHECK_OK(governor.submit_evidence(
        sample(FlowId{1}, EvidenceWindowId{2}, kStart + 10, kStart + 20, 2000, 1000, PathId{1},
               Generation::initial(), 2)));

    // Right length, wrong content: the window volume disagrees with the delta.
    const StatusOr<FlowDecision> contradiction = governor.submit_evidence(
        sample(FlowId{1}, EvidenceWindowId{3}, kStart + 20, kStart + 30, 9000, 1000, PathId{1},
               Generation::initial(), 3));
    EFG_CHECK_EQ(contradiction.code(), StatusCode::ContradictoryEvidence);
    EFG_CHECK(governor.counters().evidence_contradictory >= 1);

    // An exact replay of the window already applied is idempotent, not an error.
    const StatusOr<FlowDecision> replay = governor.submit_evidence(
        sample(FlowId{1}, EvidenceWindowId{2}, kStart + 10, kStart + 20, 2000, 1000, PathId{1},
               Generation::initial(), 2));
    EFG_CHECK_OK(replay);
    EFG_CHECK(replay.value().evidence_duplicate);
    EFG_CHECK_EQ(governor.counters().evidence_duplicates, 1u);

    // A replay of a window that has already been superseded is a contradiction:
    // the evidence stream went backwards.
    const StatusOr<FlowDecision> older = governor.submit_evidence(
        sample(FlowId{1}, EvidenceWindowId{1}, kStart, kStart + 10, 1000, 1000, PathId{1},
               Generation::initial(), 1));
    EFG_CHECK_EQ(older.code(), StatusCode::ContradictoryEvidence);
}

EFG_TEST(hostile, an_evidence_window_that_lies_about_its_own_digest_is_refused) {
    FlowSample value = sample(FlowId{1}, EvidenceWindowId{1}, kStart, kStart + 10, 1000, 1000);
    value.provenance.digest ^= 0x1ull;
    Governor governor;
    governor.set_incarnation(boot());
    install_standard(governor);
    EFG_CHECK_EQ(governor.submit_evidence(value).code(), StatusCode::IntegrityFailure);
    EFG_CHECK_EQ(governor.flow_count(), 0u);
}

EFG_TEST(hostile, a_journal_with_a_damaged_header_is_refused_rather_than_repaired) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("efg-hostile-" + unique_scratch_suffix());
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    const std::filesystem::path journal = directory / "journal.efgj";

    {
        StatusOr<Journal> created = Journal::open(journal);
        EFG_REQUIRE(created.ok());
        EFG_CHECK_STATUS_OK(created.value().close());
    }
    {
        std::FILE* file = std::fopen(journal.string().c_str(), "r+b");
        EFG_REQUIRE(file != nullptr);
        std::fputc('X', file);
        std::fclose(file);
    }
    EFG_CHECK_EQ(Journal::open(journal).code(), StatusCode::Corrupt);

    std::filesystem::remove_all(directory, error);
}

EFG_TEST(hostile, a_snapshot_with_a_damaged_checksum_is_refused) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("efg-snapshot-" + unique_scratch_suffix());
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    const std::filesystem::path snapshot = directory / "snapshot.efgs";

    const std::byte payload[32] = {};
    EFG_CHECK_STATUS_OK(SnapshotFile::write_atomic(snapshot, std::span<const std::byte>(payload, 32)));
    StatusOr<std::vector<std::byte>> read = SnapshotFile::read(snapshot);
    EFG_REQUIRE(read.ok());
    EFG_CHECK_EQ(read.value().size(), 32u);

    {
        std::FILE* file = std::fopen(snapshot.string().c_str(), "r+b");
        EFG_REQUIRE(file != nullptr);
        std::fseek(file, 24, SEEK_SET);
        std::fputc('Z', file);
        std::fclose(file);
    }
    EFG_CHECK_EQ(SnapshotFile::read(snapshot).status().code(), StatusCode::IntegrityFailure);

    {
        std::FILE* file = std::fopen(snapshot.string().c_str(), "r+b");
        EFG_REQUIRE(file != nullptr);
        std::fputc('Q', file);
        std::fclose(file);
    }
    EFG_CHECK_EQ(SnapshotFile::read(snapshot).status().code(), StatusCode::Corrupt);

    std::filesystem::remove_all(directory, error);
}

EFG_TEST(hostile, reading_a_missing_snapshot_is_not_found) {
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() /
        ("efg-does-not-exist-" + unique_scratch_suffix() + ".efgs");
    EFG_CHECK_EQ(SnapshotFile::read(missing).status().code(), StatusCode::NotFound);
    JournalReplayStats stats;
    EFG_CHECK_EQ(Journal::verify(missing, stats).code(), StatusCode::NotFound);
}

EFG_TEST(hostile, a_decoded_evidence_batch_with_trailing_bytes_is_refused) {
    std::vector<FlowSample> samples;
    samples.push_back(sample(FlowId{1}, EvidenceWindowId{1}, kStart, kStart + 10, 1000, 1000));
    samples.push_back(sample(FlowId{1}, EvidenceWindowId{2}, kStart + 10, kStart + 20, 2000, 1000));

    ByteWriter writer(1u << 16);
    EFG_CHECK_STATUS_OK(encode_evidence_batch(samples, 64, writer));
    writer.write_u8(0xAA);

    ByteReader reader(writer.buffer());
    std::vector<FlowSample> decoded;
    EFG_CHECK_EQ(decode_evidence_batch(reader, 64, decoded).code(),
                 StatusCode::ContradictoryEvidence);

    ByteReader clean(std::span<const std::byte>(writer.buffer().data(), writer.buffer().size() - 1));
    EFG_CHECK_STATUS_OK(decode_evidence_batch(clean, 64, decoded));
    EFG_CHECK_EQ(decoded.size(), 2u);
}

EFG_TEST(hostile, an_evidence_batch_beyond_the_batch_ceiling_is_refused) {
    std::vector<FlowSample> samples;
    for (u64 i = 0; i < 4; ++i) {
        samples.push_back(sample(FlowId{1}, EvidenceWindowId{i + 1}, kStart + i * 10,
                                 kStart + (i + 1) * 10, (i + 1) * 1000, 1000));
    }
    ByteWriter writer(1u << 16);
    EFG_CHECK_EQ(encode_evidence_batch(samples, 2, writer).code(), StatusCode::Oversized);

    ByteWriter small(1u << 16);
    EFG_CHECK_STATUS_OK(encode_evidence_batch(samples, 4, small));
    ByteReader reader(small.buffer());
    std::vector<FlowSample> decoded;
    EFG_CHECK_EQ(decode_evidence_batch(reader, 2, decoded).code(), StatusCode::Oversized);
}

EFG_TEST(hostile, a_non_zero_reserved_field_in_a_batch_is_refused) {
    ByteWriter writer(64);
    writer.write_u32(0);
    writer.write_u64(0xDEADBEEFULL);
    ByteReader reader(writer.buffer());
    std::vector<FlowSample> decoded;
    EFG_CHECK_EQ(decode_evidence_batch(reader, 8, decoded).code(), StatusCode::InvalidArgument);
}

EFG_TEST(hostile, a_writer_that_overflows_reports_it_rather_than_truncating_silently) {
    const FlowSample value = sample(FlowId{1}, EvidenceWindowId{1}, kStart, kStart + 10, 1000, 1000);
    ByteWriter tiny(8);
    encode(tiny, value);
    EFG_CHECK(tiny.overflowed());
}

EFG_TEST(hostile, the_governor_survives_a_flood_of_distinct_flows_up_to_its_ceiling) {
    GovernorConfig config;
    config.max_flows = 16;
    Governor governor(config);
    governor.set_incarnation(boot());
    install_standard(governor);

    std::size_t accepted = 0;
    std::size_t refused = 0;
    for (u64 id = 1; id <= 64; ++id) {
        const StatusOr<FlowDecision> decision = governor.submit_evidence(
            sample(FlowId{id}, EvidenceWindowId{1}, kStart, kStart + 10, 1000, 1000, PathId{1},
                   Generation::initial(), 1));
        if (decision.ok()) {
            ++accepted;
        } else {
            EFG_CHECK_EQ(decision.code(), StatusCode::CapacityExceeded);
            ++refused;
        }
    }
    EFG_CHECK_EQ(accepted, 16u);
    EFG_CHECK_EQ(refused, 48u);
    EFG_CHECK_EQ(governor.flow_count(), 16u);
}

EFG_TEST(hostile, hostile_labelling_is_rejected_by_the_parsers) {
    u64 value = 0;
    EFG_CHECK(!parse_u64(std::string{"18446744073709551616"}, value));  // one past the maximum
    EFG_CHECK(parse_u64(std::string{"18446744073709551615"}, value));
    EFG_CHECK_EQ(value, UINT64_MAX);
    EFG_CHECK(!is_identifier(std::string{"../etc/passwd"}));
    EFG_CHECK(!is_identifier(std::string{"a b"}));
    std::string label;
    EFG_CHECK(!parse_label(std::string(limits::kMaxNameBytes + 1, 'a'), label));
    EFG_CHECK(parse_label(std::string(limits::kMaxNameBytes, 'a'), label));
}
