// Elephant Flow Governor - logical time.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Decisions are made against logical ticks supplied by the caller, never against
// a wall clock. Wall-clock readings appear only as provenance metadata and are
// never consulted by a classification or an intent decision. That is what makes
// classification history reproducible bit for bit.

#ifndef EFG_TICK_HPP
#define EFG_TICK_HPP

#include <cstdint>

#include "efg/checked.hpp"
#include "efg/status.hpp"

namespace efg {

using Tick = std::uint64_t;

/// Half-open logical interval [begin, end).
struct TickSpan {
    Tick begin{0};
    Tick end{0};

    [[nodiscard]] constexpr bool empty() const noexcept { return end <= begin; }
    [[nodiscard]] constexpr u64 length() const noexcept { return end > begin ? end - begin : 0; }
    [[nodiscard]] constexpr bool contains(Tick t) const noexcept { return t >= begin && t < end; }
    [[nodiscard]] constexpr bool overlaps(const TickSpan& other) const noexcept {
        return begin < other.end && other.begin < end;
    }
    [[nodiscard]] constexpr bool adjoins(const TickSpan& other) const noexcept {
        return end == other.begin || other.end == begin;
    }

    friend constexpr bool operator==(const TickSpan&, const TickSpan&) noexcept = default;
};

/// Validity interval for evidence, capacity, policy and authority.
/// The interval is half-open: issued_at is inclusive, expires_at is exclusive.
struct ValidityWindow {
    Tick issued_at{0};
    Tick expires_at{0};

    [[nodiscard]] constexpr bool well_formed() const noexcept { return expires_at > issued_at; }
    [[nodiscard]] constexpr bool contains(Tick now) const noexcept {
        return well_formed() && now >= issued_at && now < expires_at;
    }
    [[nodiscard]] constexpr bool expired_at(Tick now) const noexcept {
        return well_formed() && now >= expires_at;
    }
    [[nodiscard]] constexpr bool future_at(Tick now) const noexcept { return now < issued_at; }
    [[nodiscard]] constexpr u64 length() const noexcept {
        return expires_at > issued_at ? expires_at - issued_at : 0;
    }

    friend constexpr bool operator==(const ValidityWindow&, const ValidityWindow&) noexcept = default;
};

enum class FreshnessVerdict : std::uint8_t {
    Fresh = 0,
    NotYetValid = 1,   // evidence issued in the future: clock or ordering regression
    Expired = 2,       // evidence window closed before the decision tick
    Malformed = 3,     // expires_at <= issued_at
    Regressed = 4,     // decision tick moved backwards relative to prior decisions
};

[[nodiscard]] std::string_view to_string(FreshnessVerdict verdict) noexcept;

/// Evaluate freshness of a validity window against a decision tick.
/// A malformed window is never fresh; UNKNOWN never becomes positive authority.
[[nodiscard]] constexpr FreshnessVerdict evaluate_freshness(const ValidityWindow& window,
                                                            Tick now) noexcept {
    if (!window.well_formed()) {
        return FreshnessVerdict::Malformed;
    }
    if (now < window.issued_at) {
        return FreshnessVerdict::NotYetValid;
    }
    if (now >= window.expires_at) {
        return FreshnessVerdict::Expired;
    }
    return FreshnessVerdict::Fresh;
}

[[nodiscard]] constexpr bool is_fresh(const ValidityWindow& window, Tick now) noexcept {
    return evaluate_freshness(window, now) == FreshnessVerdict::Fresh;
}

/// Build a validity window from a start tick and a duration. Refuses a zero
/// length window and refuses an interval that would overflow.
[[nodiscard]] inline StatusOr<ValidityWindow> make_validity(Tick issued_at, u64 duration) noexcept {
    if (duration == 0) {
        return make_status(StatusCode::InvalidArgument, "validity duration must be positive");
    }
    u64 expires = 0;
    if (add_overflow(issued_at, duration, expires)) {
        return make_status(StatusCode::Overflow, "validity window overflowed the tick domain");
    }
    return ValidityWindow{issued_at, expires};
}

/// Monotone guard. The runtime refuses to evaluate at a tick earlier than the
/// last tick it already committed a decision at, because doing so would make
/// classification history non-monotone and therefore non-reproducible.
class TickGuard {
public:
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] Tick last() const noexcept { return last_; }

    [[nodiscard]] Status observe(Tick now) noexcept {
        if (initialized_ && now < last_) {
            return make_status(StatusCode::Stale,
                               "decision tick regressed below the last committed tick");
        }
        initialized_ = true;
        last_ = now;
        return {};
    }

    void reset() noexcept {
        initialized_ = false;
        last_ = 0;
    }

private:
    Tick last_{0};
    bool initialized_{false};
};

}  // namespace efg

#endif  // EFG_TICK_HPP
