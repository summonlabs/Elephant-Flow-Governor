// Elephant Flow Governor - bounded text utilities.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "efg/text.hpp"

#include <limits>

#include "efg/limits.hpp"

namespace efg {
namespace {

constexpr char kQuote = static_cast<char>(34);
constexpr char kBackslash = static_cast<char>(92);

constexpr char hex_digit(unsigned value) noexcept {
    return value < 10u ? static_cast<char>('0' + static_cast<int>(value))
                       : static_cast<char>('a' + static_cast<int>(value - 10u));
}

}  // namespace

void append_u64(std::string& out, u64 value) {
    char scratch[20];
    int index = 20;
    if (value == 0) {
        out.push_back('0');
        return;
    }
    while (value != 0) {
        --index;
        scratch[index] = static_cast<char>('0' + static_cast<int>(value % 10u));
        value /= 10u;
    }
    out.append(scratch + index, static_cast<std::size_t>(20 - index));
}

void append_i64(std::string& out, i64 value) {
    if (value < 0) {
        out.push_back('-');
        const u64 magnitude = static_cast<u64>(-(value + 1)) + 1u;
        append_u64(out, magnitude);
        return;
    }
    append_u64(out, static_cast<u64>(value));
}

std::string to_decimal(u64 value) {
    std::string out;
    out.reserve(20);
    append_u64(out, value);
    return out;
}

std::string to_fixed(u64 value, unsigned decimals) {
    std::string out;
    if (decimals == 0) {
        append_u64(out, value);
        return out;
    }
    u64 scale = 1;
    for (unsigned i = 0; i < decimals; ++i) {
        scale *= 10u;
    }
    append_u64(out, value / scale);
    out.push_back('.');
    const std::string digits = to_decimal(value % scale);
    for (std::size_t i = digits.size(); i < static_cast<std::size_t>(decimals); ++i) {
        out.push_back('0');
    }
    out.append(digits);
    return out;
}

std::string to_percent(BasisPoints bp) {
    std::string out = to_fixed(static_cast<u64>(bp), 2);
    out.push_back('%');
    return out;
}

void append_json_string(std::string& out, std::string_view text) {
    out.push_back(kQuote);
    for (const char c : text) {
        const unsigned uc = static_cast<unsigned char>(c);
        switch (uc) {
            case 34u:
                out.push_back(kBackslash);
                out.push_back(kQuote);
                break;
            case 92u:
                out.push_back(kBackslash);
                out.push_back(kBackslash);
                break;
            case 8u:
                out.push_back(kBackslash);
                out.push_back('b');
                break;
            case 12u:
                out.push_back(kBackslash);
                out.push_back('f');
                break;
            case 10u:
                out.push_back(kBackslash);
                out.push_back('n');
                break;
            case 13u:
                out.push_back(kBackslash);
                out.push_back('r');
                break;
            case 9u:
                out.push_back(kBackslash);
                out.push_back('t');
                break;
            default:
                if (uc < 0x20u) {
                    out.push_back(kBackslash);
                    out.push_back('u');
                    out.push_back('0');
                    out.push_back('0');
                    out.push_back(hex_digit((uc >> 4) & 0x0Fu));
                    out.push_back(hex_digit(uc & 0x0Fu));
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back(kQuote);
}

void append_json_number(std::string& out, double value, unsigned decimals) {
    if (!(value >= 0.0) && !(value < 0.0)) {
        out.append("null");  // NaN
        return;
    }
    if (value > 1.0e300 || value < -1.0e300) {
        out.append("null");  // infinite
        return;
    }
    if (value < 0.0) {
        out.push_back('-');
        value = -value;
    }
    u64 scale = 1;
    for (unsigned i = 0; i < decimals; ++i) {
        scale *= 10u;
    }
    const double scaled = value * static_cast<double>(scale);
    if (!(scaled < 1.8e19)) {
        out.append("null");
        return;
    }
    const u64 truncated = static_cast<u64>(scaled);
    if (decimals == 0) {
        append_u64(out, truncated);
        return;
    }
    out.append(to_fixed(truncated, decimals));
}

std::string truncate_utf8(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return std::string{text};
    }
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return std::string{text.substr(0, cut)};
}

std::vector<std::string> split(std::string_view text, char delimiter) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t position = text.find(delimiter, start);
        if (position == std::string_view::npos) {
            out.emplace_back(text.substr(start));
            return out;
        }
        out.emplace_back(text.substr(start, position - start));
        start = position + 1;
    }
}

bool parse_u64(std::string_view text, u64& out) noexcept {
    if (text.empty() || text.size() > 20) {
        return false;
    }
    u64 value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const u64 digit = static_cast<u64>(c - '0');
        if (value > (std::numeric_limits<u64>::max() - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

bool parse_label(std::string_view text, std::string& out) noexcept {
    if (text.size() > limits::kMaxNameBytes) {
        return false;
    }
    for (const char c : text) {
        const unsigned uc = static_cast<unsigned char>(c);
        if (uc < 0x20u || uc == 0x7Fu) {
            return false;
        }
    }
    out.assign(text);
    return true;
}

bool is_identifier(std::string_view text) noexcept {
    if (text.empty() || text.size() > limits::kMaxNameBytes) {
        return false;
    }
    for (const char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string render_tick(Tick tick) {
    std::string out{"t"};
    append_u64(out, tick);
    return out;
}

}  // namespace efg
