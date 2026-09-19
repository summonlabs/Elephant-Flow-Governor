// Elephant Flow Governor - minimal test harness.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Deliberately dependency free: a fresh clone must configure, build and test
// with nothing but a C++20 compiler and CMake. No test timeout of any kind is
// applied anywhere: a hanging test is a defect to diagnose, not to hide.

#ifndef EFG_TEST_HARNESS_HPP
#define EFG_TEST_HARNESS_HPP

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace efgtest {

struct Failure : std::exception {
    std::string message;
    explicit Failure(std::string text) : message(std::move(text)) {}
    [[nodiscard]] const char* what() const noexcept override { return message.c_str(); }
};

struct TestCase {
    const char* suite;
    const char* name;
    void (*function)();
};

[[nodiscard]] std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* suite, const char* name, void (*function)());
};

[[noreturn]] void fail(const char* file, int line, const std::string& message);

int run_all(int argc, char** argv);

}  // namespace efgtest

#define EFG_TEST(suite_name, case_name)                                                     \
    static void suite_name##_##case_name##_body();                                          \
    static const ::efgtest::Registrar suite_name##_##case_name##_registrar{                 \
        #suite_name, #case_name, &suite_name##_##case_name##_body};                         \
    static void suite_name##_##case_name##_body()

#define EFG_FAIL(message) ::efgtest::fail(__FILE__, __LINE__, (message))

#define EFG_CHECK(condition)                                                                \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            ::efgtest::fail(__FILE__, __LINE__, "check failed: " #condition);               \
        }                                                                                   \
    } while (false)

#define EFG_REQUIRE(condition)                                                              \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            ::efgtest::fail(__FILE__, __LINE__, "requirement failed: " #condition);         \
            return;                                                                         \
        }                                                                                   \
    } while (false)

#define EFG_CHECK_EQ(lhs, rhs)                                                              \
    do {                                                                                    \
        const auto& efg_lhs_ = (lhs);                                                       \
        const auto& efg_rhs_ = (rhs);                                                       \
        if (!(efg_lhs_ == efg_rhs_)) {                                                      \
            ::efgtest::fail(__FILE__, __LINE__,                                             \
                            std::string{"expected " #lhs " == " #rhs " but "} +             \
                                ::efgtest::describe(efg_lhs_) + " != " +                    \
                                ::efgtest::describe(efg_rhs_));                             \
        }                                                                                   \
    } while (false)

#define EFG_CHECK_STATUS_OK(expression)                                                     \
    do {                                                                                    \
        const auto efg_status_ = (expression);                                              \
        if (!efg_status_.ok()) {                                                            \
            ::efgtest::fail(__FILE__, __LINE__,                                             \
                            std::string{"expected success from " #expression ": "} +        \
                                std::string{efg_status_.to_string()});                      \
        }                                                                                   \
    } while (false)

#define EFG_CHECK_STATUS_IS(expression, expected_code)                                      \
    do {                                                                                    \
        const auto efg_status_ = (expression);                                              \
        if (efg_status_.code() != (expected_code)) {                                        \
            ::efgtest::fail(__FILE__, __LINE__,                                             \
                            std::string{"expected " #expected_code " from " #expression     \
                                        " but got "} +                                      \
                                std::string{efg_status_.to_string()});                      \
        }                                                                                   \
    } while (false)

#define EFG_CHECK_OK(expression)                                                            \
    do {                                                                                    \
        auto efg_result_ = (expression);                                                    \
        if (!efg_result_.ok()) {                                                            \
            ::efgtest::fail(__FILE__, __LINE__,                                             \
                            std::string{"expected value from " #expression ": "} +          \
                                std::string{efg_result_.status().to_string()});             \
        }                                                                                   \
    } while (false)

namespace efgtest {

[[nodiscard]] std::string describe(std::uint64_t value);
[[nodiscard]] std::string describe(std::int64_t value);
[[nodiscard]] std::string describe(int value);
[[nodiscard]] std::string describe(unsigned value);
[[nodiscard]] std::string describe(bool value);
[[nodiscard]] std::string describe(const std::string& value);
[[nodiscard]] std::string describe(const char* value);
[[nodiscard]] std::string describe(const std::exception& value);

template <typename T>
[[nodiscard]] std::string describe(const T& value) {
    (void)value;
    return "<value>";
}

}  // namespace efgtest

#endif  // EFG_TEST_HARNESS_HPP
