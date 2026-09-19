// Elephant Flow Governor - test harness implementation.
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "test_harness.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace efgtest {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

Registrar::Registrar(const char* suite, const char* name, void (*function)()) {
    registry().push_back(TestCase{suite, name, function});
}

void fail(const char* file, int line, const std::string& message) {
    std::string text{file};
    text.push_back(':');
    text.append(std::to_string(line));
    text.append(": ");
    text.append(message);
    throw Failure{text};
}

std::string describe(std::uint64_t value) { return std::to_string(value); }
std::string describe(std::int64_t value) { return std::to_string(value); }
std::string describe(int value) { return std::to_string(value); }
std::string describe(unsigned value) { return std::to_string(value); }
std::string describe(bool value) { return value ? "true" : "false"; }
std::string describe(const std::string& value) { return value; }
std::string describe(const char* value) { return value == nullptr ? "<null>" : std::string{value}; }
std::string describe(const std::exception& value) { return value.what(); }

int run_all(int argc, char** argv) {
    const char* filter = nullptr;
    bool list = false;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            list = true;
        } else if (std::strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[i + 1];
            ++i;
        }
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    for (const TestCase& test : registry()) {
        std::string full{test.suite};
        full.push_back('.');
        full.append(test.name);
        if (filter != nullptr && full.find(filter) == std::string::npos) {
            continue;
        }
        if (list) {
            std::printf("%s\n", full.c_str());
            continue;
        }
        if (verbose) {
            std::printf("RUN  %s\n", full.c_str());
            std::fflush(stdout);
        }
        try {
            test.function();
            ++passed;
        } catch (const Failure& failure) {
            ++failed;
            std::printf("FAIL %s\n  %s\n", full.c_str(), failure.message.c_str());
        } catch (const std::exception& error) {
            ++failed;
            std::printf("FAIL %s\n  unexpected exception: %s\n", full.c_str(), error.what());
        } catch (...) {
            ++failed;
            std::printf("FAIL %s\n  unexpected non-standard exception\n", full.c_str());
        }
        std::fflush(stdout);
    }
    if (list) {
        return 0;
    }
    std::printf("%zu passed, %zu failed\n", passed, failed);
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}

}  // namespace efgtest

int main(int argc, char** argv) { return ::efgtest::run_all(argc, argv); }
