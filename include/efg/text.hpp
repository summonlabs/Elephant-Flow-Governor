// Elephant Flow Governor - bounded text utilities.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Formatting is deliberately hand rolled: no locale, no iostream width state, no
// floating point formatting differences between platforms. The rendered output
// of a decision is therefore byte identical everywhere.

#ifndef EFG_TEXT_HPP
#define EFG_TEXT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "efg/checked.hpp"
#include "efg/tick.hpp"

namespace efg {

/// Append the decimal representation of an unsigned value.
void append_u64(std::string& out, u64 value);
void append_i64(std::string& out, i64 value);

/// Render an unsigned value as decimal.
[[nodiscard]] std::string to_decimal(u64 value);

/// Render a fixed point value with the supplied number of decimal places,
/// truncating rather than rounding so the result is monotone in the input.
[[nodiscard]] std::string to_fixed(u64 value, unsigned decimals);

/// Render basis points as a percentage with two decimals, for example "12.34%".
[[nodiscard]] std::string to_percent(BasisPoints bp);

/// Append a JSON escaped string literal, including the surrounding quotes.
void append_json_string(std::string& out, std::string_view text);

/// Append a floating point value truncated to the supplied number of decimals.
void append_json_number(std::string& out, double value, unsigned decimals);

/// Truncate a string to at most max_bytes bytes on a UTF-8 boundary.
[[nodiscard]] std::string truncate_utf8(std::string_view text, std::size_t max_bytes);

/// Split on a single delimiter. Empty fields are preserved.
[[nodiscard]] std::vector<std::string> split(std::string_view text, char delimiter);

/// Parse an unsigned integer. Rejects empty input, signs, whitespace and
/// anything that overflows.
[[nodiscard]] bool parse_u64(std::string_view text, u64& out) noexcept;

/// Parse a bounded opaque label.
[[nodiscard]] bool parse_label(std::string_view text, std::string& out) noexcept;

/// True when the name is a plain identifier: letters, digits, '_' and '-'.
[[nodiscard]] bool is_identifier(std::string_view text) noexcept;

/// Render a tick as a stable, human readable token.
[[nodiscard]] std::string render_tick(Tick tick);

}  // namespace efg

#endif  // EFG_TEXT_HPP
