// Elephant Flow Governor - canonical encoding.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// One place where every durable and on-the-wire structure is written and read.
// Encodings are little endian, length prefixed where variable and strictly bounds
// checked on the way back in. A decoder never trusts a length it read from the
// wire: it refuses anything that would exceed the caller supplied ceiling.

#ifndef EFG_CODEC_HPP
#define EFG_CODEC_HPP

#include <span>

#include "efg/capacity.hpp"
#include "efg/classification.hpp"
#include "efg/flow.hpp"
#include "efg/governor.hpp"
#include "efg/hash.hpp"
#include "efg/impact.hpp"
#include "efg/intervention.hpp"
#include "efg/path.hpp"
#include "efg/policy.hpp"
#include "efg/status.hpp"
#include "efg/tick.hpp"

namespace efg {

// --- Simple value types ----------------------------------------------------
void encode(ByteWriter& writer, const TickSpan& span);
[[nodiscard]] Status decode(ByteReader& reader, TickSpan& span);
void encode(ByteWriter& writer, const ValidityWindow& window);
[[nodiscard]] Status decode(ByteReader& reader, ValidityWindow& window);
void encode(ByteWriter& writer, const Provenance& provenance);
[[nodiscard]] Status decode(ByteReader& reader, Provenance& provenance);

// --- Thresholds, rules and policy ------------------------------------------
void encode(ByteWriter& writer, const Threshold& threshold);
[[nodiscard]] Status decode(ByteReader& reader, Threshold& threshold);
void encode(ByteWriter& writer, const RuleNode& node);
[[nodiscard]] Status decode(ByteReader& reader, RuleNode& node);
void encode(ByteWriter& writer, const RuleSet& rules);
[[nodiscard]] Status decode(ByteReader& reader, RuleSet& rules);
void encode(ByteWriter& writer, const HysteresisSpec& spec);
[[nodiscard]] Status decode(ByteReader& reader, HysteresisSpec& spec);
void encode(ByteWriter& writer, const IntentAuthorization& authorization);
[[nodiscard]] Status decode(ByteReader& reader, IntentAuthorization& authorization);
void encode(ByteWriter& writer, const PolicyDocument& policy);
[[nodiscard]] Status decode(ByteReader& reader, PolicyDocument& policy);

// --- Evidence --------------------------------------------------------------
void encode(ByteWriter& writer, const FlowSample& sample);
[[nodiscard]] Status decode(ByteReader& reader, FlowSample& sample);
void encode(ByteWriter& writer, const FlowMeasurements& measurements);
[[nodiscard]] Status decode(ByteReader& reader, FlowMeasurements& measurements);
void encode(ByteWriter& writer, const ResourceCapacity& capacity);
[[nodiscard]] Status decode(ByteReader& reader, ResourceCapacity& capacity);
void encode(ByteWriter& writer, const PathDescriptor& path);
[[nodiscard]] Status decode(ByteReader& reader, PathDescriptor& path);

// --- Decisions -------------------------------------------------------------
void encode(ByteWriter& writer, const ImpactAssessment& impact);
[[nodiscard]] Status decode(ByteReader& reader, ImpactAssessment& impact);
void encode(ByteWriter& writer, const AuthorityVector& authority);
[[nodiscard]] Status decode(ByteReader& reader, AuthorityVector& authority);
void encode(ByteWriter& writer, const ThresholdTraceEntry& entry);
[[nodiscard]] Status decode(ByteReader& reader, ThresholdTraceEntry& entry);
void encode(ByteWriter& writer, const FlowClassification& classification);
[[nodiscard]] Status decode(ByteReader& reader, FlowClassification& classification);
void encode(ByteWriter& writer, const ClassificationTransition& transition);
[[nodiscard]] Status decode(ByteReader& reader, ClassificationTransition& transition);
void encode(ByteWriter& writer, const IntentBounds& bounds);
[[nodiscard]] Status decode(ByteReader& reader, IntentBounds& bounds);
void encode(ByteWriter& writer, const GovernanceIntent& intent);
[[nodiscard]] Status decode(ByteReader& reader, GovernanceIntent& intent);

}  // namespace efg

#endif  // EFG_CODEC_HPP
