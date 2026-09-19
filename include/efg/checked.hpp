// Elephant Flow Governor - checked arithmetic.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Every externally influenced size, capacity, rate, counter, share and time
// unit is manipulated through these helpers. Silently wrapping arithmetic is
// never used for a value that can reach a decision.

#ifndef EFG_CHECKED_HPP
#define EFG_CHECKED_HPP

#include <cstdint>
#include <limits>

#include "efg/status.hpp"

namespace efg {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

/// Basis points: one ten-thousandth. 10000 == 1.0. Shares are integral so that
/// classification decisions are bit-identical across platforms.
using BasisPoints = std::uint32_t;

inline constexpr BasisPoints kBasisPointsScale = 10000u;

/// Bytes per one thousand logical ticks. Rates are integral for the same reason.
using KiloTickRate = std::uint64_t;
inline constexpr u64 kKiloTickScale = 1000u;

[[nodiscard]] constexpr bool add_overflow(u64 a, u64 b, u64& out) noexcept {
    if (a > std::numeric_limits<u64>::max() - b) {
        return true;
    }
    out = a + b;
    return false;
}

[[nodiscard]] constexpr bool sub_overflow(u64 a, u64 b, u64& out) noexcept {
    if (b > a) {
        return true;
    }
    out = a - b;
    return false;
}

[[nodiscard]] constexpr bool mul_overflow(u64 a, u64 b, u64& out) noexcept {
    if (a != 0 && b > std::numeric_limits<u64>::max() / a) {
        return true;
    }
    out = a * b;
    return false;
}

/// 64x64 -> 128 bit widening multiply expressed with 32 bit limbs.
constexpr void widening_mul(u64 a, u64 b, u64& hi, u64& lo) noexcept {
    const u64 a_lo = a & 0xFFFFFFFFull;
    const u64 a_hi = a >> 32;
    const u64 b_lo = b & 0xFFFFFFFFull;
    const u64 b_hi = b >> 32;

    const u64 p0 = a_lo * b_lo;
    const u64 p1 = a_lo * b_hi;
    const u64 p2 = a_hi * b_lo;
    const u64 p3 = a_hi * b_hi;

    const u64 mid = (p0 >> 32) + (p1 & 0xFFFFFFFFull) + (p2 & 0xFFFFFFFFull);
    lo = (mid << 32) | (p0 & 0xFFFFFFFFull);
    hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

/// Exact floor((hi:lo) / divisor) for a 128 bit dividend. Returns false when the
/// quotient does not fit in 64 bits or the divisor is zero.
constexpr bool divmod128(u64 hi, u64 lo, u64 divisor, u64& quotient, u64& remainder) noexcept {
    if (divisor == 0) {
        return false;
    }
    if (hi == 0) {
        quotient = lo / divisor;
        remainder = lo % divisor;
        return true;
    }
    if (hi >= divisor) {
        return false;  // quotient would exceed 64 bits
    }
    u64 q = 0;
    u64 r = hi;
    for (int bit = 127; bit >= 0; --bit) {
        const u64 next_bit = (bit >= 64) ? ((hi >> (bit - 64)) & 1ull) : ((lo >> bit) & 1ull);
        const u64 carry = r >> 63;  // bit shifted out of the remainder
        r = ((r << 1) | next_bit) & std::numeric_limits<u64>::max();
        if (carry != 0 || r >= divisor) {
            r -= divisor;
            if (bit < 64) {
                q |= (1ull << bit);
            } else {
                return false;  // quotient exceeded 64 bits
            }
        }
    }
    quotient = q;
    remainder = r;
    return true;
}

/// Exact floor(a * b / divisor) with overflow detection.
[[nodiscard]] constexpr bool mul_div(u64 a, u64 b, u64 divisor, u64& out) noexcept {
    if (divisor == 0) {
        return false;
    }
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    // Prefer an exact division before multiplying to keep the common path cheap.
    // The negated mul_overflow is deliberate: mul_div reports success, while
    // mul_overflow reports the failure.
    if (a % divisor == 0) {
        return !mul_overflow(a / divisor, b, out);
    }
    if (b % divisor == 0) {
        return !mul_overflow(b / divisor, a, out);
    }
    u64 hi = 0;
    u64 lo = 0;
    widening_mul(a, b, hi, lo);
    u64 remainder = 0;
    return divmod128(hi, lo, divisor, out, remainder);
}

/// Saturating addition. Used where a ceiling is preferable to a refusal, for
/// example when accumulating contention pressure across competing flows.
[[nodiscard]] constexpr u64 saturating_add(u64 a, u64 b) noexcept {
    u64 out = 0;
    if (add_overflow(a, b, out)) {
        return std::numeric_limits<u64>::max();
    }
    return out;
}

/// Saturating multiplication.
[[nodiscard]] constexpr u64 saturating_mul(u64 a, u64 b) noexcept {
    u64 out = 0;
    if (mul_overflow(a, b, out)) {
        return std::numeric_limits<u64>::max();
    }
    return out;
}

/// Clamp helper used by every bounded governance intent.
[[nodiscard]] constexpr u64 clamp_to(u64 value, u64 ceiling) noexcept {
    return value > ceiling ? ceiling : value;
}

[[nodiscard]] constexpr u64 min_of(u64 a, u64 b) noexcept { return a < b ? a : b; }
[[nodiscard]] constexpr u64 max_of(u64 a, u64 b) noexcept { return a > b ? a : b; }

/// Convert a ratio expressed in basis points of a total into an absolute value.
/// Returns false on overflow.
[[nodiscard]] constexpr bool apply_basis_points(u64 total, BasisPoints bp, u64& out) noexcept {
    if (bp > kBasisPointsScale) {
        return false;
    }
    return mul_div(total, static_cast<u64>(bp), static_cast<u64>(kBasisPointsScale), out);
}

/// Expression of "part of whole" in basis points, saturating at 10000.
[[nodiscard]] constexpr BasisPoints to_basis_points(u64 part, u64 whole) noexcept {
    if (whole == 0) {
        return 0;
    }
    if (part >= whole) {
        return kBasisPointsScale;
    }
    u64 out = 0;
    if (!mul_div(part, static_cast<u64>(kBasisPointsScale), whole, out)) {
        return kBasisPointsScale;
    }
    return static_cast<BasisPoints>(out > kBasisPointsScale ? kBasisPointsScale : out);
}

/// Checked downcast of an externally supplied 64 bit value into a 32 bit field.
[[nodiscard]] constexpr bool narrow_u32(u64 value, u32& out) noexcept {
    if (value > std::numeric_limits<u32>::max()) {
        return false;
    }
    out = static_cast<u32>(value);
    return true;
}

/// Failing-checked variants that produce Status directly, for API boundaries.
[[nodiscard]] inline StatusOr<u64> checked_add(u64 a, u64 b) noexcept {
    u64 out = 0;
    if (add_overflow(a, b, out)) {
        return make_status(StatusCode::Overflow, "addition overflowed 64 bits");
    }
    return out;
}

[[nodiscard]] inline StatusOr<u64> checked_sub(u64 a, u64 b) noexcept {
    u64 out = 0;
    if (sub_overflow(a, b, out)) {
        return make_status(StatusCode::Underflow, "subtraction underflowed 64 bits");
    }
    return out;
}

[[nodiscard]] inline StatusOr<u64> checked_mul(u64 a, u64 b) noexcept {
    u64 out = 0;
    if (mul_overflow(a, b, out)) {
        return make_status(StatusCode::Overflow, "multiplication overflowed 64 bits");
    }
    return out;
}

[[nodiscard]] inline StatusOr<u64> checked_mul_div(u64 a, u64 b, u64 divisor) noexcept {
    u64 out = 0;
    if (divisor == 0) {
        return make_status(StatusCode::DivideByZero, "zero divisor in scaled computation");
    }
    if (!mul_div(a, b, divisor, out)) {
        return make_status(StatusCode::Overflow, "scaled computation overflowed 64 bits");
    }
    return out;
}

}  // namespace efg

#endif  // EFG_CHECKED_HPP
