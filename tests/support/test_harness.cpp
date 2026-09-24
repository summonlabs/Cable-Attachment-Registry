// Cable Attachment Registry — test harness implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace crtest {
namespace {

struct Case {
  std::string suite;
  std::string name;
  TestFn fn;
};

std::vector<Case>& cases() {
  static std::vector<Case> registry;
  return registry;
}

std::map<std::string, std::string>& options() {
  static std::map<std::string, std::string> captured;
  return captured;
}

std::string qualified(const Case& item) {
  return item.suite + "." + item.name;
}

struct CaseFailure {
  std::string message;
};

} // namespace

void register_case(const char* suite, const char* name, TestFn fn) {
  cases().push_back(Case{suite, name, fn});
}

void fail(const char* file, int line, const std::string& message) {
  throw CaseFailure{std::string(file) + ":" + std::to_string(line) + ": " + message};
}

const std::string& option(const std::string& name) {
  static const std::string empty;
  const auto found = options().find(name);
  return found == options().end() ? empty : found->second;
}

int run_all(int argc, char** argv) {
  bool list = false;
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      list = true;
      continue;
    }
    if (argument.size() > 2 && argument[0] == '-' && argument[1] == '-') {
      const std::string name = argument.substr(2);
      if (index + 1 < argc) {
        options()[name] = argv[index + 1];
        ++index;
      } else {
        options()[name] = std::string();
      }
      continue;
    }
    filter = argument;
  }

  std::vector<Case> ordered = cases();
  std::stable_sort(ordered.begin(), ordered.end(), [](const Case& left, const Case& right) {
    return qualified(left) < qualified(right);
  });

  if (list) {
    for (const Case& item : ordered) {
      std::printf("%s\n", qualified(item).c_str());
    }
    return 0;
  }

  int failed = 0;
  int executed = 0;
  for (const Case& item : ordered) {
    const std::string name = qualified(item);
    if (!filter.empty() && name.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    const auto start = std::chrono::steady_clock::now();
    try {
      item.fn();
    } catch (const CaseFailure& failure) {
      ++failed;
      std::printf("FAIL %s\n  %s\n", name.c_str(), failure.message.c_str());
      std::fflush(stdout);
      continue;
    } catch (const std::exception& error) {
      ++failed;
      std::printf("FAIL %s\n  unexpected exception: %s\n", name.c_str(), error.what());
      std::fflush(stdout);
      continue;
    } catch (...) {
      ++failed;
      std::printf("FAIL %s\n  unexpected non-standard exception\n", name.c_str());
      std::fflush(stdout);
      continue;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    std::printf("ok   %s (%lld ms)\n", name.c_str(), static_cast<long long>(elapsed));
    std::fflush(stdout);
  }

  std::printf("%d case(s) run, %d failed\n", executed, failed);
  std::fflush(stdout);
  return failed == 0 ? 0 : 1;
}

Rng::Rng(std::uint64_t seed) noexcept : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed), seed_(seed) {}

std::uint64_t Rng::next_u64() noexcept {
  state_ += 0x9E3779B97F4A7C15ull;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

std::uint32_t Rng::next_u32() noexcept {
  return static_cast<std::uint32_t>(next_u64() >> 32);
}

std::uint64_t Rng::below(std::uint64_t bound) noexcept {
  if (bound == 0) {
    return 0;
  }
  return next_u64() % bound;
}

std::int64_t Rng::in_range(std::int64_t lo, std::int64_t hi) noexcept {
  if (hi <= lo) {
    return lo;
  }
  const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1;
  return lo + static_cast<std::int64_t>(below(span));
}

bool Rng::chance(std::uint32_t percent) noexcept {
  if (percent == 0) {
    return false;
  }
  if (percent >= 100) {
    return true;
  }
  return below(100) < percent;
}

std::string random_text(Rng& rng, std::size_t length) {
  static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::string text(length, 'a');
  for (std::size_t index = 0; index < length; ++index) {
    text[index] = kAlphabet[rng.below(sizeof(kAlphabet) - 1)];
  }
  return text;
}

TempDir::TempDir(std::string_view tag) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
  std::filesystem::path base = std::filesystem::temp_directory_path();
  path_ = (base / ("cable-registry-test-" + std::string(tag) + "-" + std::to_string(now) + "-" +
                   std::to_string(thread)))
              .string();
  std::error_code code;
  std::filesystem::create_directories(path_, code);
}

TempDir::~TempDir() {
  std::error_code code;
  std::filesystem::remove_all(path_, code);
}

std::string TempDir::file(std::string_view name) const {
  return (std::filesystem::path(path_) / std::string(name)).string();
}

} // namespace crtest

int main(int argc, char** argv) {
  return crtest::run_all(argc, argv);
}
