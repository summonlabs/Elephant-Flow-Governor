// Elephant Flow Governor - canonical encoding round trip tests.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <vector>

#include "efg/efg.hpp"
#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace efg;
using namespace efgtest;

namespace {

template <typename T>
void round_trip(const T& value) {
    ByteWriter writer(1u << 20);
    encode(writer, value);
    EFG_REQUIRE(!writer.overflowed());
    ByteReader reader(writer.buffer());
    T decoded{};
    const Status status = decode(reader, decoded);
    EFG_CHECK(status.ok());
    EFG_CHECK(reader.at_end());

    ByteWriter again(1u << 20);
    encode(again, decoded);
    EFG_CHECK(again.buffer() == writer.buffer());
}

}  // namespace

EFG_TEST(codec, flow_sample_round_trips_byte_for_byte) {
    const FlowSample value = sample(FlowId{4}, EvidenceWindowId{9}, 100, 140, 4096, 4096, PathId{2},
                                    Generation{3}, 7, Generation{5});
    round_trip(value);
    ByteWriter writer(1024);
    encode(writer, value);
    ByteReader reader(writer.buffer());
    FlowSample decoded;
    EFG_CHECK_STATUS_OK(decode(reader, decoded));
    EFG_CHECK_EQ(decoded.flow.value(), 4u);
    EFG_CHECK_EQ(decoded.flow_generation.value(), 5u);
    EFG_CHECK_EQ(decoded.path_generation.value(), 3u);
    EFG_CHECK_EQ(decoded.window_sequence, 7u);
    EFG_CHECK_EQ(decoded.window_bytes, 4096u);
    EFG_CHECK_EQ(decoded.provenance.digest, value.provenance.digest);
}

EFG_TEST(codec, capacity_and_path_round_trip) {
    round_trip(capacity(ResourceId{2}, 5000, 500, kStart, 5000, Generation{4}, 9));
    round_trip(path(PathId{3}, {ResourceId{1}, ResourceId{2}, ResourceId{3}}, kStart, 5000,
                    Generation{2}));
}

EFG_TEST(codec, policy_round_trips_including_the_rule_tree) {
    const PolicyDocument policy = default_policy();
    round_trip(policy);
    ByteWriter writer(1u << 16);
    encode(writer, policy);
    ByteReader reader(writer.buffer());
    PolicyDocument decoded;
    EFG_CHECK_STATUS_OK(decode(reader, decoded));
    EFG_CHECK_EQ(decoded.digest, policy.digest);
    EFG_CHECK_STATUS_OK(decoded.validate());
    EFG_CHECK_EQ(decoded.enter_rule.nodes.size(), policy.enter_rule.nodes.size());
}

EFG_TEST(codec, classification_round_trips_with_its_authority_vector) {
    Governor governor;
    const BootIdentity identity = boot(3, 4);
    governor.set_incarnation(identity);
    install_standard(governor);
    feed_constant(governor, FlowId{1}, 3000, 10, 3);

    StatusOr<FlowClassification> classification =
        governor.classification(FlowId{1}, Generation::initial(), kStart + 30);
    EFG_REQUIRE(classification.ok());
    round_trip(classification.value());

    ByteWriter writer(1u << 16);
    encode(writer, classification.value());
    ByteReader reader(writer.buffer());
    FlowClassification decoded;
    EFG_CHECK_STATUS_OK(decode(reader, decoded));
    EFG_CHECK_EQ(decoded.digest, classification.value().digest);
    EFG_CHECK_EQ(decoded.authority_vector.digest(), classification.value().authority_vector.digest());
    EFG_CHECK_EQ(decoded.state, classification.value().state);
}

EFG_TEST(codec, intent_round_trips_with_bounds_and_window) {
    GovernanceIntent intent;
    intent.id = IntentId{11};
    intent.attempt = AttemptId{22};
    intent.kind = IntentKind::RequestRateShaping;
    intent.flow = FlowId{3};
    intent.flow_generation = Generation::initial();
    intent.classification = ClassificationId{44};
    intent.classification_generation = Generation{2};
    intent.state = IntentState::Clamped;
    intent.suppressed = SuppressReason::None;
    intent.requested.rate_reduction_bp = 6000;
    intent.requested.duration_ticks = 100;
    intent.granted.rate_reduction_bp = 2500;
    intent.granted.duration_ticks = 100;
    intent.severity = 4321;
    intent.window = TickSpan{1000, 1100};
    intent.authority.epoch = EpochId{2};
    round_trip(intent);
}

EFG_TEST(codec, truncated_input_is_rejected_rather_than_partially_applied) {
    const FlowSample value = sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000);
    ByteWriter writer(1024);
    encode(writer, value);
    for (std::size_t cut = 0; cut < writer.buffer().size(); cut += 7) {
        ByteReader reader(std::span<const std::byte>(writer.buffer().data(), cut));
        FlowSample decoded;
        const Status status = decode(reader, decoded);
        EFG_CHECK(!status.ok());
    }
}

EFG_TEST(codec, trailing_bytes_are_detected_by_the_caller_not_silently_ignored) {
    const FlowSample value = sample(FlowId{1}, EvidenceWindowId{1}, 100, 110, 1000, 1000);
    ByteWriter writer(1024);
    encode(writer, value);
    writer.write_u64(0xDEADBEEFull);
    ByteReader reader(writer.buffer());
    FlowSample decoded;
    EFG_CHECK_STATUS_OK(decode(reader, decoded));
    EFG_CHECK(!reader.at_end());
}

EFG_TEST(codec, an_oversized_collection_length_is_refused_before_allocation) {
    ByteWriter writer(64);
    writer.write_u32(0xFFFFFFFFu);
    writer.write_u32(0);
    ByteReader reader(writer.buffer());
    RuleSet rules;
    EFG_CHECK_EQ(decode(reader, rules).code(), StatusCode::Oversized);
}

EFG_TEST(codec, an_unrecognized_enumeration_value_is_refused) {
    ByteWriter writer(32);
    writer.write_u8(200);  // threshold kind far outside the enumeration
    ByteReader reader(writer.buffer());
    Threshold threshold;
    EFG_CHECK_EQ(decode(reader, threshold).code(), StatusCode::Corrupt);
}
