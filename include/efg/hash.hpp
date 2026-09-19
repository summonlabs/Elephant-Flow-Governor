// Elephant Flow Governor - deterministic hashing and integrity checks.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Two distinct facilities live here and they are deliberately not interchangeable:
//
//  * stable_hash64 / Hasher - a portable, platform independent digest used for
//    identity, determinism proofs and classification history digests. It is not
//    a cryptographic primitive and never claims to be one.
//  * crc32c - a table driven Castagnoli CRC used to detect damaged or truncated
//    durable records and framed transport payloads.

#ifndef EFG_HASH_HPP
#define EFG_HASH_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "efg/checked.hpp"

namespace efg {

inline constexpr u64 kFnvOffsetBasis = 0xCBF29CE484222325ull;
inline constexpr u64 kFnvPrime = 0x00000100000001B3ull;

/// Incremental FNV-1a 64 bit digest with a final avalanche step so that small
/// structured inputs do not cluster in the low bits.
class Hasher {
public:
    constexpr Hasher() noexcept = default;
    constexpr explicit Hasher(u64 seed) noexcept : state_(seed) {}

    constexpr void write_byte(u8 value) noexcept {
        state_ ^= static_cast<u64>(value);
        state_ *= kFnvPrime;
    }

    void write(std::span<const std::byte> bytes) noexcept {
        for (const std::byte b : bytes) {
            write_byte(static_cast<u8>(b));
        }
    }

    void write(std::string_view text) noexcept {
        for (const char c : text) {
            write_byte(static_cast<u8>(c));
        }
    }

    /// Feed an unsigned value using its decimal rendering. Used where a digest
    /// must agree with a text rendering of the same field.
    void write_decimal(u64 value) noexcept {
        char scratch[20];
        int index = 20;
        if (value == 0) {
            write_byte(static_cast<u8>('0'));
            return;
        }
        while (value != 0) {
            --index;
            scratch[index] = static_cast<char>('0' + static_cast<int>(value % 10u));
            value /= 10u;
        }
        for (int i = index; i < 20; ++i) {
            write_byte(static_cast<u8>(scratch[i]));
        }
    }

    constexpr void write_u8(u8 value) noexcept { write_byte(value); }

    constexpr void write_u16(u16 value) noexcept {
        write_byte(static_cast<u8>(value & 0xFFu));
        write_byte(static_cast<u8>((value >> 8) & 0xFFu));
    }

    constexpr void write_u32(u32 value) noexcept {
        for (int shift = 0; shift < 32; shift += 8) {
            write_byte(static_cast<u8>((value >> shift) & 0xFFu));
        }
    }

    constexpr void write_u64(u64 value) noexcept {
        for (int shift = 0; shift < 64; shift += 8) {
            write_byte(static_cast<u8>((value >> shift) & 0xFFu));
        }
    }

    constexpr void write_bool(bool value) noexcept { write_byte(value ? u8{1} : u8{0}); }

    /// Feed another digest as an opaque 64 bit token.
    constexpr void write_digest(u64 digest) noexcept { write_u64(digest); }

    /// Finish the digest. Mixing is a 64 bit finalizer (splitmix-style) so that
    /// the mapping from state to published digest is a bijection.
    [[nodiscard]] constexpr u64 finish() const noexcept {
        u64 z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    [[nodiscard]] constexpr u64 state() const noexcept { return state_; }

private:
    u64 state_{kFnvOffsetBasis};
};

/// One-shot digest over an arbitrary byte span.
[[nodiscard]] inline u64 stable_hash64(std::span<const std::byte> bytes) noexcept {
    Hasher hasher;
    hasher.write(bytes);
    return hasher.finish();
}

/// One-shot digest over a string.
[[nodiscard]] inline u64 stable_hash64(std::string_view text) noexcept {
    Hasher hasher;
    hasher.write(text);
    return hasher.finish();
}

/// Castagnoli CRC-32C. Software implementation with a lazily built table so the
/// result is identical on every platform and never depends on CPU features.
[[nodiscard]] u32 crc32c(std::span<const std::byte> bytes) noexcept;

/// Incremental CRC-32C for streaming verification.
class Crc32c {
public:
    constexpr Crc32c() noexcept = default;
    void update(std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] u32 value() const noexcept { return value_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    u32 value_{0};
    std::size_t size_{0};
};

/// Lightweight byte writer used by the canonical encoders. It refuses to grow
/// past a caller supplied ceiling instead of allocating without bound.
///
/// The write methods deliberately return bool without [[nodiscard]]: an encoder
/// performs a long uninterrupted run of writes and latches the first failure in
/// the writer itself. The caller checks overflowed() once, after the run, which
/// makes a silently truncated encoding impossible without cluttering every
/// encoder with error plumbing.
class ByteWriter {
public:
    explicit ByteWriter(std::size_t ceiling) : ceiling_(ceiling) {}

    bool write_u8(u8 value);
    bool write_u16(u16 value);
    bool write_u32(u32 value);
    bool write_u64(u64 value);
    bool write_bool(bool value);
    bool write_bytes(std::span<const std::byte> bytes);
    bool write_string(std::string_view text);

    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
    [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<std::byte> take() noexcept { return std::move(buffer_); }

private:
    [[nodiscard]] bool reserve(std::size_t additional);

    std::vector<std::byte> buffer_;
    std::size_t ceiling_;
    bool overflowed_{false};
};

/// Sequential reader with strict bounds checking. Every accessor fails rather
/// than reading partially outside the supplied span.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] bool read_u8(u8& out) noexcept;
    [[nodiscard]] bool read_u16(u16& out) noexcept;
    [[nodiscard]] bool read_u32(u32& out) noexcept;
    [[nodiscard]] bool read_u64(u64& out) noexcept;
    [[nodiscard]] bool read_bool(bool& out) noexcept;
    [[nodiscard]] bool read_bytes(std::size_t count, std::span<const std::byte>& out) noexcept;
    [[nodiscard]] bool read_string(std::size_t max_bytes, std::string& out);

    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] bool at_end() const noexcept { return offset_ == data_.size(); }

private:
    std::span<const std::byte> data_;
    std::size_t offset_{0};
};

}  // namespace efg

#endif  // EFG_HASH_HPP
