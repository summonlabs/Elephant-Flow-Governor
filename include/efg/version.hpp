// Elephant Flow Governor - version and build identity.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef EFG_VERSION_HPP
#define EFG_VERSION_HPP

#include <cstdint>
#include <string_view>

#define EFG_VERSION_MAJOR 1
#define EFG_VERSION_MINOR 0
#define EFG_VERSION_PATCH 0

/// On-disk schema revision for durable state and wire frames. Bumped whenever
/// the persisted or framed representation changes incompatibly.
#define EFG_SCHEMA_VERSION 1u

namespace efg {

inline constexpr std::uint32_t kVersionMajor = EFG_VERSION_MAJOR;
inline constexpr std::uint32_t kVersionMinor = EFG_VERSION_MINOR;
inline constexpr std::uint32_t kVersionPatch = EFG_VERSION_PATCH;
inline constexpr std::uint32_t kSchemaVersion = EFG_SCHEMA_VERSION;

/// Semantic version string of the runtime.
[[nodiscard]] constexpr std::string_view version_string() noexcept { return "1.0.0"; }

/// Human readable product name. Used by tools and by the package metadata.
[[nodiscard]] constexpr std::string_view product_name() noexcept { return "Elephant Flow Governor"; }

}  // namespace efg

#endif  // EFG_VERSION_HPP
