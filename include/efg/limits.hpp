// Elephant Flow Governor - hard bounds.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every externally influenced population, buffer, queue, record, payload and
// explanation is bounded here. Nothing in the runtime grows without a ceiling.

#ifndef EFG_LIMITS_HPP
#define EFG_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace efg::limits {

// --- Populations -----------------------------------------------------------
inline constexpr std::size_t kMaxFlows = 1u << 20;              // 1,048,576 live flow generations
inline constexpr std::size_t kMaxPaths = 1u << 16;              // 65,536 path generations
inline constexpr std::size_t kMaxResources = 1u << 16;          // 65,536 capacity resources
inline constexpr std::size_t kMaxPolicies = 1u << 12;           // 4,096 retained policy generations
inline constexpr std::size_t kMaxIntents = 1u << 18;            // 262,144 retained governance intents
inline constexpr std::size_t kMaxAttempts = 1u << 20;           // idempotency records for intents
inline constexpr std::size_t kMaxWorkers = 1u << 10;            // 1,024 concurrent cluster workers

// --- Per-flow evidence -----------------------------------------------------
inline constexpr std::size_t kMaxSamplesPerFlow = 32;           // retained evidence windows per flow
inline constexpr std::size_t kMaxHistoryPerFlow = 512;          // retained classification transitions
inline constexpr std::size_t kMaxResourcesPerPath = 64;         // resources bound to one path
inline constexpr std::size_t kMaxTraceEntries = 16;             // retained threshold evaluations per decision

// --- Policy ---------------------------------------------------------------
inline constexpr std::size_t kMaxPolicyNodes = 4096;            // flattened rule nodes per rule set
inline constexpr std::size_t kMaxRuleDepth = 32;                // nesting depth of a composed rule
inline constexpr std::size_t kMaxPolicyBytes = 1u << 20;        // serialized policy ceiling

// --- Explanation ----------------------------------------------------------
inline constexpr std::size_t kMaxExplanationLines = 48;         // bounded explanation record
inline constexpr std::size_t kMaxExplanationText = 8192;        // bounded rendered summary bytes
inline constexpr std::size_t kMaxIntentListPerFlow = 16;        // bounded intent listing

// --- Transport ------------------------------------------------------------
inline constexpr std::size_t kMaxFramePayload = 1u << 20;       // 1 MiB framed payload ceiling
inline constexpr std::size_t kMaxFrameBytes = kMaxFramePayload + 64;
inline constexpr std::size_t kMaxEvidenceBatch = 4096;          // samples per framed evidence batch
inline constexpr std::size_t kMaxNameBytes = 128;               // bounded opaque label

// --- Durable state --------------------------------------------------------
inline constexpr std::size_t kMaxJournalRecordBytes = 1u << 20; // 1 MiB per journal record
inline constexpr std::size_t kMaxSnapshotBytes = 1u << 28;      // 256 MiB snapshot ceiling
inline constexpr std::size_t kMaxJournalReplayRecords = 1u << 22;  // 4,194,304 replayed records
inline constexpr std::uint64_t kMaxJournalFileBytes = 1ull << 34;  // 16 GiB hard file ceiling

// --- Service queues -------------------------------------------------------
inline constexpr std::size_t kDefaultQueueCapacity = 1u << 14;  // 16,384 queued requests
inline constexpr std::size_t kMaxQueueCapacity = 1u << 20;

// --- Benchmark ------------------------------------------------------------
inline constexpr std::size_t kMaxBenchmarkPopulation = 1u << 22;  // 4,194,304 synthetic flows

/// Returns true when the value fits inside the configured ceiling.
[[nodiscard]] constexpr bool within(std::size_t value, std::size_t ceiling) noexcept {
    return value <= ceiling;
}

}  // namespace efg::limits

#endif  // EFG_LIMITS_HPP
