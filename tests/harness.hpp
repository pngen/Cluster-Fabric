// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Minimal zero-dependency test harness.
//
// The repository must build and test from a fresh clone with no network access
// and no third-party test framework, so the harness is deliberately small:
// self-registering cases, typed failures, and an exit code that is zero only
// when every case passed. There is no timeout, no watchdog and no test that
// passes by terminating.

#ifndef CLUSTER_FABRIC_TESTS_HARNESS_HPP
#define CLUSTER_FABRIC_TESTS_HARNESS_HPP

#include <cmath>
#include <cstddef>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace cf_test {

struct Failure {
  std::string message;
};

using TestBody = void (*)();

struct TestCase {
  const char* name;
  TestBody body;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

struct Registrar {
  Registrar(const char* name, TestBody body) { registry().push_back(TestCase{name, body}); }
};

/// Renders a value for failure messages. Streamable values, values with str()
/// (strong counters) and values with value() (strong identities) are all
/// supported, as are enums that provide to_string.
template <class T>
std::string text_of(const T& value) {
  if constexpr (std::is_enum_v<T> && requires(const T& v) { to_string(v); }) {
    return std::string(to_string(value));
  } else if constexpr (std::is_enum_v<T>) {
    return "<enum>";
  } else if constexpr (requires(const T& v) { v.str(); }) {
    return std::string(value.str());
  } else if constexpr (requires(const T& v) { v.value(); }) {
    return std::string(value.value());
  } else if constexpr (requires(std::ostream& os, const T& v) { os << v; }) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else {
    return "<value>";
  }
}

template <class T>
std::string text_of(const std::optional<T>& value) {
  return value.has_value() ? text_of(*value) : std::string("<absent>");
}

inline int run(const char* suite) {
  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const TestCase& test : registry()) {
    try {
      test.body();
      ++passed;
      std::cout << "[PASS] " << test.name << "\n";
    } catch (const Failure& failure) {
      ++failed;
      std::cout << "[FAIL] " << test.name << ": " << failure.message << "\n";
    } catch (const std::exception& error) {
      ++failed;
      std::cout << "[FAIL] " << test.name << ": unexpected exception: " << error.what() << "\n";
    } catch (...) {
      ++failed;
      std::cout << "[FAIL] " << test.name << ": unexpected non-standard exception\n";
    }
  }
  std::cout << suite << ": " << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}

}  // namespace cf_test

#define CF_TEST(name)                                                            \
  static void cf_test_case_##name();                                             \
  static const ::cf_test::Registrar cf_test_registrar_##name(#name,              \
                                                             &cf_test_case_##name); \
  static void cf_test_case_##name()

#define CF_FAIL(message)                                                         \
  throw ::cf_test::Failure{std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                           ": " + (message)}

#define CF_EXPECT(condition)                          \
  do {                                                \
    if (!(condition)) {                               \
      CF_FAIL("expectation failed: " #condition);      \
    }                                                 \
  } while (false)

// The operands are copied rather than bound to a reference: a reference bound to
// a subobject of a temporary (for example *optional_of_temporary) can outlive
// the temporary and silently compare equal empty values.
#define CF_EXPECT_EQ(actual, expected)                                             \
  do {                                                                             \
    const auto cf_actual = (actual);                                               \
    const auto cf_expected = (expected);                                           \
    if (!(cf_actual == cf_expected)) {                                             \
      CF_FAIL(std::string("expected ") + #actual + " == " + #expected +            \
              " (actual " + ::cf_test::text_of(cf_actual) + ", expected " +        \
              ::cf_test::text_of(cf_expected) + ")");                              \
    }                                                                              \
  } while (false)

#define CF_EXPECT_NE(actual, unexpected)                                            \
  do {                                                                             \
    const auto cf_actual = (actual);                                               \
    const auto cf_unexpected = (unexpected);                                       \
    if (cf_actual == cf_unexpected) {                                              \
      CF_FAIL(std::string("expected ") + #actual + " != " + #unexpected +          \
              " (both " + ::cf_test::text_of(cf_actual) + ")");                    \
    }                                                                              \
  } while (false)

#define CF_EXPECT_NEAR(actual, expected, tolerance)                                       \
  do {                                                                                   \
    const double cf_actual = static_cast<double>(actual);                                \
    const double cf_expected = static_cast<double>(expected);                            \
    const double cf_tolerance = static_cast<double>(tolerance);                          \
    if (!(std::fabs(cf_actual - cf_expected) <= cf_tolerance)) {                          \
      CF_FAIL(std::string("expected ") + #actual + " near " + #expected + " (actual " +    \
              std::to_string(cf_actual) + ", expected " + std::to_string(cf_expected) +    \
              ", tolerance " + std::to_string(cf_tolerance) + ")");                       \
    }                                                                                    \
  } while (false)

#define CF_EXPECT_THROWS(expression)                                             \
  do {                                                                           \
    bool cf_threw = false;                                                       \
    try {                                                                        \
      (void)(expression);                                                        \
    } catch (...) {                                                              \
      cf_threw = true;                                                           \
    }                                                                            \
    if (!cf_threw) {                                                             \
      CF_FAIL("expected exception from " #expression);                            \
    }                                                                            \
  } while (false)

#endif  // CLUSTER_FABRIC_TESTS_HARNESS_HPP
