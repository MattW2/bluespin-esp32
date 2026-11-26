#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace test_harness {

struct TestCase {
  const char *name;
  std::function<void()> fn;
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int &failure_count() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const char *name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void check_impl(bool cond, const char *expr, const char *file, int line, bool fatal) {
  if (!cond) {
    ++failure_count();
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, expr);
    if (fatal) {
      std::abort();
    }
  }
}

inline int run_all() {
  int passed = 0;
  int failed_cases = 0;
  for (const auto &tc : registry()) {
    const int before = failure_count();
    std::printf("[ RUN      ] %s\n", tc.name);
    tc.fn();
    if (failure_count() == before) {
      std::printf("[       OK ] %s\n", tc.name);
      ++passed;
    } else {
      std::printf("[  FAILED  ] %s\n", tc.name);
      ++failed_cases;
    }
  }
  std::printf("\nSummary: %d passed, %d failed (%d assertion failures)\n", passed, failed_cases, failure_count());
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace test_harness

#define CONCAT_INNER(a, b) a##b
#define CONCAT(a, b) CONCAT_INNER(a, b)

#define TEST_CASE(name) \
  static void CONCAT(test_fn_, __LINE__)(); \
  static ::test_harness::Registrar CONCAT(test_reg_, __LINE__)(name, CONCAT(test_fn_, __LINE__)); \
  static void CONCAT(test_fn_, __LINE__)()

#define CHECK(expr) ::test_harness::check_impl(static_cast<bool>(expr), #expr, __FILE__, __LINE__, false)
#define REQUIRE(expr) ::test_harness::check_impl(static_cast<bool>(expr), #expr, __FILE__, __LINE__, true)
