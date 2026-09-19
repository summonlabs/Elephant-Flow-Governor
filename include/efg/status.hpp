// Elephant Flow Governor - status and result plumbing.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The runtime never throws across an API boundary. Every fallible entry point
// returns a Status or StatusOr<T>, and every failure names a precise reason so
// that refusals remain explainable.

#ifndef EFG_STATUS_HPP
#define EFG_STATUS_HPP

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace efg {

enum class StatusCode : std::uint16_t {
    Ok = 0,
    InvalidArgument,
    OutOfRange,
    Overflow,
    Underflow,
    DivideByZero,
    NotFound,
    AlreadyExists,
    Conflict,
    CapacityExceeded,
    Unsupported,
    Stale,
    Expired,
    UnknownEvidence,
    ContradictoryEvidence,
    DuplicateEvidence,
    GapDetected,
    GenerationMismatch,
    AuthorityMismatch,
    EpochMismatch,
    BootMismatch,
    RevalidationRequired,
    PolicyViolation,
    ProtectionViolation,
    Corrupt,
    Truncated,
    Oversized,
    VersionMismatch,
    IntegrityFailure,
    IoError,
    Closed,
    Cancelled,
    ShuttingDown,
    Busy,
    Internal,
};

/// Stable machine readable name of a status code.
[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

/// True when the code denotes success.
[[nodiscard]] constexpr bool is_ok(StatusCode code) noexcept {
    return code == StatusCode::Ok;
}

/// True when the condition is transient and a later submission may succeed.
[[nodiscard]] constexpr bool is_retryable(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Busy:
        case StatusCode::CapacityExceeded:
        case StatusCode::Stale:
        case StatusCode::Expired:
        case StatusCode::GapDetected:
        case StatusCode::RevalidationRequired:
        case StatusCode::UnknownEvidence:
            return true;
        default:
            return false;
    }
}

/// True when the condition indicates damaged or hostile input.
[[nodiscard]] constexpr bool is_integrity_failure(StatusCode code) noexcept {
    switch (code) {
        case StatusCode::Corrupt:
        case StatusCode::Truncated:
        case StatusCode::Oversized:
        case StatusCode::VersionMismatch:
        case StatusCode::IntegrityFailure:
        case StatusCode::ContradictoryEvidence:
            return true;
        default:
            return false;
    }
}

class Status {
public:
    constexpr Status() noexcept = default;

    constexpr Status(StatusCode code, std::string_view message) noexcept
        : code_(code), message_(message) {}

    explicit constexpr Status(StatusCode code) noexcept : code_(code) {}

    [[nodiscard]] constexpr StatusCode code() const noexcept { return code_; }
    [[nodiscard]] constexpr bool ok() const noexcept { return code_ == StatusCode::Ok; }
    [[nodiscard]] constexpr std::string_view message() const noexcept { return message_; }

    /// Rendered form, bounded by the fixed message literal.
    [[nodiscard]] std::string to_string() const;

    friend constexpr bool operator==(const Status& lhs, StatusCode rhs) noexcept {
        return lhs.code_ == rhs;
    }
    friend constexpr bool operator==(const Status& lhs, const Status& rhs) noexcept {
        return lhs.code_ == rhs.code_;
    }
    friend constexpr bool operator!=(const Status& lhs, const Status& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    StatusCode code_{StatusCode::Ok};
    std::string_view message_{};
};

/// Convenience constructors used throughout the runtime.
[[nodiscard]] constexpr Status ok_status() noexcept { return Status{}; }
[[nodiscard]] constexpr Status make_status(StatusCode code, std::string_view message) noexcept {
    return Status{code, message};
}

/// Result carrier. Either holds a value or a non-ok Status.
template <typename T>
class [[nodiscard]] StatusOr {
public:
    using value_type = T;

    StatusOr(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    StatusOr(Status status) : storage_(std::in_place_index<1>, status) {}

    template <typename U = T, std::enable_if_t<std::is_convertible_v<U, T>, int> = 0>
    StatusOr(U&& value) : storage_(std::in_place_index<0>, T(std::forward<U>(value))) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] StatusCode code() const noexcept {
        return ok() ? StatusCode::Ok : std::get<1>(storage_).code();
    }
    [[nodiscard]] const Status& status() const noexcept {
        static const Status kOk{};
        return ok() ? kOk : std::get<1>(storage_);
    }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::move(std::get<0>(storage_)); }

    [[nodiscard]] T& operator*() & { return value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }
    [[nodiscard]] T* operator->() { return &value(); }
    [[nodiscard]] const T* operator->() const { return &value(); }

    /// Value or a caller supplied default when the result is a failure.
    [[nodiscard]] T value_or(T fallback) const {
        return ok() ? value() : std::move(fallback);
    }

private:
    std::variant<T, Status> storage_;
};

/// Propagate a failing Status out of the enclosing function.
#define EFG_TRY(expr)                                    \
    do {                                                 \
        const ::efg::Status efg_try_status_ = (expr);    \
        if (!efg_try_status_.ok()) {                     \
            return efg_try_status_;                      \
        }                                                \
    } while (false)

/// Propagate a failing StatusOr out of the enclosing function, binding value.
#define EFG_TRY_ASSIGN(name, expr)                       \
    auto efg_try_tmp_##name = (expr);                    \
    if (!efg_try_tmp_##name.ok()) {                      \
        return efg_try_tmp_##name.status();              \
    }                                                    \
    auto& name = efg_try_tmp_##name.value()

/// Propagate a failing StatusOr out of the enclosing function without binding.
#define EFG_TRY_VOID(expr)                               \
    do {                                                 \
        auto efg_try_tmp_void_ = (expr);                 \
        if (!efg_try_tmp_void_.ok()) {                   \
            return efg_try_tmp_void_.status();           \
        }                                                \
    } while (false)

}  // namespace efg

#endif  // EFG_STATUS_HPP
