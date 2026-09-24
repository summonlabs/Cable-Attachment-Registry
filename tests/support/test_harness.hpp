// Cable Attachment Registry — minimal dependency-free test harness.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every test translation unit includes this header; the definitions live in
// tests/support/test_harness.cpp. There is deliberately no test framework and
// no timeout: a case that hangs is a defect in the code under test, and the
// harness must not turn that into a pass.

#ifndef CABLE_REGISTRY_TESTS_SUPPORT_TEST_HARNESS_HPP
#define CABLE_REGISTRY_TESTS_SUPPORT_TEST_HARNESS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace crtest {

using TestFn = void (*)();

/// Registers one case. Safe to call from a static initializer.
void register_case(const char* suite, const char* name, TestFn fn);

/// Records the failure of the running case and aborts it. Never returns.
[[noreturn]] void fail(const char* file, int line, const std::string& message);

/// Runs every registered case, or only the cases whose "suite.name" contains
/// the filter when one is given. "--list" prints the names instead. Returns
/// zero when no case failed.
///
/// Arguments of the form "--name value" are captured before the filter is
/// chosen, so a suite that drives external processes can read them with
/// option() without every test executable defining its own main.
int run_all(int argc, char** argv);

/// The value of a captured "--name value" argument, or the empty string.
const std::string& option(const std::string& name);

/// SplitMix64. Deterministic for a given seed.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept;

  std::uint64_t next_u64() noexcept;
  std::uint32_t next_u32() noexcept;
  /// Uniform value in [0, bound). The bound must be greater than zero.
  std::uint64_t below(std::uint64_t bound) noexcept;
  /// Uniform value in [lo, hi]. Returns lo when hi is not greater than lo.
  std::int64_t in_range(std::int64_t lo, std::int64_t hi) noexcept;
  /// True with probability percent/100.
  bool chance(std::uint32_t percent) noexcept;
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_;
  std::uint64_t seed_;
};

/// Deterministic lowercase alphanumeric text of exactly the requested length.
std::string random_text(Rng& rng, std::size_t length);

/// A temporary directory removed when the object goes out of scope.
class TempDir {
 public:
  explicit TempDir(std::string_view tag);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  /// A path inside the temporary directory.
  [[nodiscard]] std::string file(std::string_view name) const;

 private:
  std::string path_;
};

} // namespace crtest

#define CR_TEST_CASE(suite, name)                                                 \
  static void crtest_case_##suite##_##name();                                     \
  namespace {                                                                     \
  const bool crtest_registered_##suite##_##name =                                 \
      (::crtest::register_case(#suite, #name, &crtest_case_##suite##_##name), true); \
  } /* namespace */                                                               \
  static void crtest_case_##suite##_##name()

#define CR_CHECK(expr)                                                          \
  do {                                                                          \
    if (!(expr)) {                                                              \
      ::crtest::fail(__FILE__, __LINE__, std::string("CR_CHECK failed: ") + #expr); \
    }                                                                           \
  } while (false)

#define CR_CHECK_EQ(actual, expected)                                                          \
  do {                                                                                         \
    const auto& crtest_actual = (actual);                                                       \
    const auto& crtest_expected = (expected);                                                   \
    if (!(crtest_actual == crtest_expected)) {                                                  \
      ::crtest::fail(__FILE__, __LINE__,                                                        \
                     std::string("CR_CHECK_EQ failed: ") + #actual + " != " + #expected);       \
    }                                                                                          \
  } while (false)

#define CR_CHECK_MSG(expr, message)                                                       \
  do {                                                                                    \
    if (!(expr)) {                                                                        \
      ::crtest::fail(__FILE__, __LINE__, std::string("CR_CHECK failed: ") + #expr + ": " + \
                                              std::string(message));                      \
    }                                                                                     \
  } while (false)

#endif // CABLE_REGISTRY_TESTS_SUPPORT_TEST_HARNESS_HPP
