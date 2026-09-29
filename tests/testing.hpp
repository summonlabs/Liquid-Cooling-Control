#pragma once

// Minimal dependency-free test harness.
//
// Cases are registered at static-initialisation time and selected by group name
// on the command line.  There is no timeout machinery anywhere in the suite: a
// hanging case is a defect to be diagnosed, not something to be worked around.

#include <cstdio>
#include <cstdlib>
#include <type_traits>

#include "liquidcooling/liquidcooling.hpp"
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lcctest {

struct TestCase final {
    std::string name;
    std::string group;
    std::function<void()> body;
};

class Registry final {
public:
    static std::vector<TestCase>& cases() {
        static std::vector<TestCase> instance;
        return instance;
    }

    static std::size_t& failure_count() {
        static std::size_t instance = 0;
        return instance;
    }

    static std::string& current_case() {
        static std::string instance;
        return instance;
    }

    static void record_failure(std::string_view message, const char* file, int line) {
        ++failure_count();
        std::fprintf(stderr, "FAIL %s\n  %s:%d: %s\n", current_case().c_str(), file, line,
                     std::string(message).c_str());
    }

    static void note(std::string_view message) {
        std::fprintf(stdout, "  note %s: %s\n", current_case().c_str(), std::string(message).c_str());
    }
};

class Registrar final {
public:
    Registrar(const char* group, const char* name, std::function<void()> body) {
        Registry::cases().push_back(TestCase{name, group, std::move(body)});
    }
};

template <typename T>
std::string describe_value(const T& value) {
    if constexpr (std::is_enum_v<T>) {
        if constexpr (requires { liquidcooling::to_string(value); }) {
            return std::string(liquidcooling::to_string(value));
        } else {
            return std::to_string(static_cast<long long>(value));
        }
    } else if constexpr (requires { std::to_string(value); }) {
        return std::to_string(value);
    } else if constexpr (requires { value.to_display_string(); }) {
        return value.to_display_string();
    } else if constexpr (requires { value.value(); }) {
        return std::to_string(value.value());
    } else {
        return std::string("<value>");
    }
}

inline std::string describe_value(const std::string& value) { return value; }
inline std::string describe_value(std::string_view value) { return std::string(value); }
inline std::string describe_value(const char* value) { return std::string(value); }
inline std::string describe_value(bool value) { return value ? "true" : "false"; }

/// Compares two values without letting the compiler fold the comparison into a
/// constant condition.  Scalar operands are read through volatile pointers so
/// that a comparison of two compile-time constants still produces a real branch;
/// MSVC otherwise rejects the check with "conditional expression is constant".
/// This is deliberately local to the test harness and suppresses nothing.
template <typename A, typename B>
[[nodiscard]] bool values_equal(const A& a, const B& b) noexcept {
    if constexpr (std::is_scalar_v<A> && std::is_scalar_v<B>) {
        const volatile A* left = &a;
        const volatile B* right = &b;
        return *left == *right;
    } else {
        return a == b;
    }
}

int run(std::string_view group_filter, std::string_view case_filter);

}  // namespace lcctest

#define LCC_TEST_CONCAT_INNER(a, b) a##b
#define LCC_TEST_CONCAT(a, b) LCC_TEST_CONCAT_INNER(a, b)

#define TEST(group, name)                                                                        \
    static void LCC_TEST_CONCAT(lcc_case_, __LINE__)();                                          \
    static const ::lcctest::Registrar LCC_TEST_CONCAT(lcc_reg_, __LINE__)(                        \
        #group, #name, LCC_TEST_CONCAT(lcc_case_, __LINE__));                                     \
    static void LCC_TEST_CONCAT(lcc_case_, __LINE__)()

#define CHECK(condition)                                                                          \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            ::lcctest::Registry::record_failure("CHECK failed: " #condition, __FILE__, __LINE__); \
        }                                                                                         \
    } while (false)

// The operands are materialised by value.  Binding a reference to an expression
// such as f().value().member would leave the reference dangling, because the
// lifetime of the temporary returned by f() is not extended through the call to
// value(); AddressSanitizer found exactly that in an earlier revision of this
// harness.
#define CHECK_EQ(actual, expected)                                                                \
    do {                                                                                          \
        const auto lcc_actual = (actual);                                                         \
        const auto lcc_expected = (expected);                                                     \
        if (!::lcctest::values_equal(lcc_actual, lcc_expected)) {                                 \
            ::lcctest::Registry::record_failure(                                                  \
                std::string("CHECK_EQ failed: " #actual " == " #expected " (actual=") +           \
                    ::lcctest::describe_value(lcc_actual) + " expected=" +                        \
                    ::lcctest::describe_value(lcc_expected) + ")",                                \
                __FILE__, __LINE__);                                                              \
        }                                                                                         \
    } while (false)

#define CHECK_CODE(result, expected_code)                                                         \
    do {                                                                                          \
        auto&& lcc_result = (result);                                                             \
        if (lcc_result.code() != (expected_code)) {                                               \
            ::lcctest::Registry::record_failure(                                                  \
                std::string("CHECK_CODE failed: " #result " yielded ") +                          \
                    std::string(::liquidcooling::to_string(lcc_result.code())) + " (" +           \
                    lcc_result.status().message() + ") expected " +                               \
                    std::string(::liquidcooling::to_string(expected_code)),                       \
                __FILE__, __LINE__);                                                              \
        }                                                                                         \
    } while (false)

#define REQUIRE(condition)                                                                        \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            ::lcctest::Registry::record_failure("REQUIRE failed: " #condition, __FILE__, __LINE__);\
            return;                                                                               \
        }                                                                                         \
    } while (false)

#define REQUIRE_CODE(result, expected_code)                                                       \
    do {                                                                                          \
        auto&& lcc_result = (result);                                                             \
        if (lcc_result.code() != (expected_code)) {                                               \
            ::lcctest::Registry::record_failure(                                                  \
                std::string("REQUIRE_CODE failed: " #result " yielded ") +                        \
                    std::string(::liquidcooling::to_string(lcc_result.code())) + " (" +           \
                    lcc_result.status().message() + ") expected " +                               \
                    std::string(::liquidcooling::to_string(expected_code)),                       \
                __FILE__, __LINE__);                                                              \
            return;                                                                               \
        }                                                                                         \
    } while (false)
