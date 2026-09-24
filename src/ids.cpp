// Cable Attachment Registry — identity formatting and parsing.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/ids.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

namespace cable_registry {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

/// Value of one hexadecimal digit, or -1.
int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

/// True at the positions a canonical 8-4-4-4-12 UUID puts a dash.
bool is_uuid_separator(std::size_t index) noexcept {
  return index == 8 || index == 13 || index == 18 || index == 23;
}

} // namespace

std::string to_hex(const Id128& value) {
  std::string text(32, '0');
  // 128 bits is 32 hexadecimal digits: the high half fills the first sixteen,
  // the low half the last sixteen, each written most significant nibble first.
  for (int index = 0; index < 32; ++index) {
    const int shift = 60 - ((index % 16) * 4);
    const std::uint64_t half = (index < 16) ? value.hi : value.lo;
    const auto nibble = static_cast<std::uint8_t>((half >> shift) & 0xFu);
    text[static_cast<std::size_t>(index)] = kHexDigits[nibble];
  }
  return text;
}

bool parse_hex_id(std::string_view text, Id128& out) noexcept {
  std::array<std::uint8_t, 32> digits{};
  std::size_t count = 0;
  std::size_t position = 0;
  for (const char character : text) {
    if (character == '-') {
      if (!is_uuid_separator(position)) {
        return false;
      }
      ++position;
      continue;
    }
    if (count >= digits.size()) {
      return false;
    }
    const int value = hex_value(character);
    if (value < 0) {
      return false;
    }
    digits[count] = static_cast<std::uint8_t>(value);
    ++count;
    ++position;
  }
  if (count != digits.size()) {
    return false;
  }
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  for (std::size_t index = 0; index < 16; ++index) {
    hi = (hi << 4) | digits[index];
  }
  for (std::size_t index = 16; index < 32; ++index) {
    lo = (lo << 4) | digits[index];
  }
  out = Id128{hi, lo};
  return true;
}

namespace detail {
namespace {

/// A per-process random engine, seeded from the entropy source plus a clock
/// and a thread identifier so that two registries started in the same
/// millisecond still differ.
std::mt19937_64& engine() noexcept {
  static thread_local std::mt19937_64 instance = [] {
    std::random_device device;
    std::seed_seq seed{static_cast<std::uint64_t>(device()),
                       static_cast<std::uint64_t>(device()),
                       static_cast<std::uint64_t>(
                           std::chrono::steady_clock::now().time_since_epoch().count()),
                       static_cast<std::uint64_t>(
                           std::hash<std::thread::id>{}(std::this_thread::get_id()))};
    return std::mt19937_64(seed);
  }();
  return instance;
}

} // namespace

Id128 random_id128() noexcept {
  std::mt19937_64& source = engine();
  Id128 value{source(), source()};
  if (value.is_nil()) {
    // Probability 2^-128, but a nil identity is never valid, so it is removed
    // rather than left to be rejected somewhere downstream.
    value.lo = 1;
  }
  return value;
}

} // namespace detail

} // namespace cable_registry
