// Elephant Flow Governor - hashing and byte level codecs.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/hash.hpp"

#include <array>
#include <cstring>

namespace efg {
namespace {

constexpr u32 kCrc32cPolynomial = 0x82F63B78u;  // reflected Castagnoli polynomial

struct Crc32cTable {
    // A plain array rather than std::array: the analysis pass cannot discharge
    // the standard library's subscript precondition inside a constexpr
    // constructor, and a plain array keeps the bound syntactically evident.
    u32 entries[256]{};

    constexpr Crc32cTable() noexcept {
        for (std::size_t index = 0; index < 256; ++index) {
            u32 crc = static_cast<u32>(index);
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc & 1u) != 0u ? ((crc >> 1) ^ kCrc32cPolynomial) : (crc >> 1);
            }
            entries[index] = crc;
        }
    }
};

constexpr Crc32cTable kCrcTable{};

}  // namespace

u32 crc32c(std::span<const std::byte> bytes) noexcept {
    u32 crc = 0xFFFFFFFFu;
    for (const std::byte b : bytes) {
        // The index is the low byte of the running CRC, masked explicitly so the
        // 0..255 bound is visible to a reader and to a static analyser.
        const u8 index = static_cast<u8>((crc ^ static_cast<u32>(b)) & 0xFFu);
        crc = kCrcTable.entries[index] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void Crc32c::update(std::span<const std::byte> bytes) noexcept {
    u32 crc = value_ ^ 0xFFFFFFFFu;
    for (const std::byte b : bytes) {
        const u8 index = static_cast<u8>((crc ^ static_cast<u32>(b)) & 0xFFu);
        crc = kCrcTable.entries[index] ^ (crc >> 8);
    }
    value_ = crc ^ 0xFFFFFFFFu;
    size_ += bytes.size();
}

bool ByteWriter::reserve(std::size_t additional) {
    if (overflowed_) {
        return false;
    }
    if (additional > ceiling_ || buffer_.size() > ceiling_ - additional) {
        overflowed_ = true;
        return false;
    }
    buffer_.reserve(buffer_.size() + additional);
    return true;
}

bool ByteWriter::write_u8(u8 value) {
    if (!reserve(1)) {
        return false;
    }
    buffer_.push_back(static_cast<std::byte>(value));
    return true;
}

bool ByteWriter::write_u16(u16 value) {
    if (!reserve(2)) {
        return false;
    }
    for (int shift = 0; shift < 16; shift += 8) {
        buffer_.push_back(static_cast<std::byte>(static_cast<u8>((value >> shift) & 0xFFu)));
    }
    return true;
}

bool ByteWriter::write_u32(u32 value) {
    if (!reserve(4)) {
        return false;
    }
    for (int shift = 0; shift < 32; shift += 8) {
        buffer_.push_back(static_cast<std::byte>(static_cast<u8>((value >> shift) & 0xFFu)));
    }
    return true;
}

bool ByteWriter::write_u64(u64 value) {
    if (!reserve(8)) {
        return false;
    }
    for (int shift = 0; shift < 64; shift += 8) {
        buffer_.push_back(static_cast<std::byte>(static_cast<u8>((value >> shift) & 0xFFu)));
    }
    return true;
}

bool ByteWriter::write_bool(bool value) { return write_u8(value ? u8{1} : u8{0}); }

bool ByteWriter::write_bytes(std::span<const std::byte> bytes) {
    if (!reserve(bytes.size())) {
        return false;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    return true;
}

bool ByteWriter::write_string(std::string_view text) {
    if (text.size() > 0xFFFFFFFFull) {
        overflowed_ = true;
        return false;
    }
    if (!write_u32(static_cast<u32>(text.size()))) {
        return false;
    }
    if (!reserve(text.size())) {
        return false;
    }
    for (const char c : text) {
        buffer_.push_back(static_cast<std::byte>(static_cast<u8>(c)));
    }
    return true;
}

bool ByteReader::read_u8(u8& out) noexcept {
    if (remaining() < 1) {
        return false;
    }
    out = static_cast<u8>(data_[offset_]);
    offset_ += 1;
    return true;
}

bool ByteReader::read_u16(u16& out) noexcept {
    if (remaining() < 2) {
        return false;
    }
    u16 value = 0;
    for (int i = 0; i < 2; ++i) {
        value |= static_cast<u16>(static_cast<u16>(static_cast<u8>(data_[offset_ + static_cast<std::size_t>(i)]))
                                  << (8 * i));
    }
    offset_ += 2;
    out = value;
    return true;
}

bool ByteReader::read_u32(u32& out) noexcept {
    if (remaining() < 4) {
        return false;
    }
    u32 value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<u32>(static_cast<u8>(data_[offset_ + static_cast<std::size_t>(i)]))
                 << (8 * i);
    }
    offset_ += 4;
    out = value;
    return true;
}

bool ByteReader::read_u64(u64& out) noexcept {
    if (remaining() < 8) {
        return false;
    }
    u64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<u64>(static_cast<u8>(data_[offset_ + static_cast<std::size_t>(i)]))
                 << (8 * i);
    }
    offset_ += 8;
    out = value;
    return true;
}

bool ByteReader::read_bool(bool& out) noexcept {
    u8 raw = 0;
    if (!read_u8(raw)) {
        return false;
    }
    if (raw > 1) {
        return false;
    }
    out = raw == 1;
    return true;
}

bool ByteReader::read_bytes(std::size_t count, std::span<const std::byte>& out) noexcept {
    if (remaining() < count) {
        return false;
    }
    out = data_.subspan(offset_, count);
    offset_ += count;
    return true;
}

bool ByteReader::read_string(std::size_t max_bytes, std::string& out) {
    u32 length = 0;
    if (!read_u32(length)) {
        return false;
    }
    if (static_cast<std::size_t>(length) > max_bytes) {
        return false;
    }
    std::span<const std::byte> raw;
    if (!read_bytes(static_cast<std::size_t>(length), raw)) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    return true;
}

}  // namespace efg
