// Cable Attachment Registry — bounded binary encoding primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/codec.hpp"

#include <cstring>
#include <limits>

namespace cable_registry {
namespace {

void append_bytes(std::vector<std::byte>& buffer, const void* data, std::size_t count) {
  const auto* first = static_cast<const std::byte*>(data);
  buffer.insert(buffer.end(), first, first + count);
}

} // namespace

void Encoder::reserve_for(std::size_t extra) {
  if (failed_) {
    return;
  }
  const std::size_t limit = kMaxEncodedBytes;
  if (extra > limit || buffer_.size() > limit - extra) {
    fail();
    return;
  }
  const std::size_t needed = buffer_.size() + extra;
  if (needed <= buffer_.capacity()) {
    return;
  }
  // Grow geometrically. Reserving exactly what the next write needs would
  // reallocate on almost every write and turn encoding a large value into
  // quadratic copying, which is exactly what a snapshot of a large graph is.
  std::size_t grown = buffer_.capacity() == 0 ? 256 : buffer_.capacity();
  while (grown < needed) {
    if (grown > limit / 2) {
      grown = limit;
      break;
    }
    grown *= 2;
  }
  if (grown > limit) {
    grown = limit;
  }
  buffer_.reserve(grown);
}

void Encoder::u8(std::uint8_t value) {
  reserve_for(1);
  if (failed_) {
    return;
  }
  buffer_.push_back(static_cast<std::byte>(value));
}

void Encoder::u16(std::uint16_t value) {
  reserve_for(2);
  if (failed_) {
    return;
  }
  const std::uint8_t raw[2] = {static_cast<std::uint8_t>(value & 0xFFu),
                               static_cast<std::uint8_t>((value >> 8) & 0xFFu)};
  append_bytes(buffer_, raw, 2);
}

void Encoder::u32(std::uint32_t value) {
  reserve_for(4);
  if (failed_) {
    return;
  }
  const std::uint8_t raw[4] = {static_cast<std::uint8_t>(value & 0xFFu),
                               static_cast<std::uint8_t>((value >> 8) & 0xFFu),
                               static_cast<std::uint8_t>((value >> 16) & 0xFFu),
                               static_cast<std::uint8_t>((value >> 24) & 0xFFu)};
  append_bytes(buffer_, raw, 4);
}

void Encoder::u64(std::uint64_t value) {
  reserve_for(8);
  if (failed_) {
    return;
  }
  std::uint8_t raw[8] = {};
  for (int index = 0; index < 8; ++index) {
    raw[index] = static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu);
  }
  append_bytes(buffer_, raw, 8);
}

void Encoder::i64(std::int64_t value) {
  u64(static_cast<std::uint64_t>(value));
}

void Encoder::boolean(bool value) {
  u8(value ? 1u : 0u);
}

void Encoder::id128(const Id128& value) {
  u64(value.hi);
  u64(value.lo);
}

void Encoder::bytes(std::span<const std::byte> value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    fail();
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  if (failed_) {
    return;
  }
  reserve_for(value.size());
  if (failed_) {
    return;
  }
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void Encoder::string(std::string_view value, std::uint32_t max_bytes) {
  if (value.size() > max_bytes || value.size() > std::numeric_limits<std::uint32_t>::max()) {
    fail();
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  if (failed_) {
    return;
  }
  reserve_for(value.size());
  if (failed_) {
    return;
  }
  append_bytes(buffer_, value.data(), value.size());
}

bool Decoder::take(std::size_t count, std::span<const std::byte>& out) noexcept {
  if (count > data_.size() - offset_) {
    return false;
  }
  out = data_.subspan(offset_, count);
  offset_ += count;
  return true;
}

bool Decoder::u8(std::uint8_t& out) noexcept {
  std::span<const std::byte> raw;
  if (!take(1, raw)) {
    return false;
  }
  out = static_cast<std::uint8_t>(raw[0]);
  return true;
}

bool Decoder::u16(std::uint16_t& out) noexcept {
  std::span<const std::byte> raw;
  if (!take(2, raw)) {
    return false;
  }
  out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(raw[0]) |
                                   (static_cast<std::uint16_t>(raw[1]) << 8));
  return true;
}

bool Decoder::u32(std::uint32_t& out) noexcept {
  std::span<const std::byte> raw;
  if (!take(4, raw)) {
    return false;
  }
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(raw[static_cast<std::size_t>(index)]) << (index * 8);
  }
  out = value;
  return true;
}

bool Decoder::u64(std::uint64_t& out) noexcept {
  std::span<const std::byte> raw;
  if (!take(8, raw)) {
    return false;
  }
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(raw[static_cast<std::size_t>(index)]) << (index * 8);
  }
  out = value;
  return true;
}

bool Decoder::i64(std::int64_t& out) noexcept {
  std::uint64_t value = 0;
  if (!u64(value)) {
    return false;
  }
  out = static_cast<std::int64_t>(value);
  return true;
}

bool Decoder::boolean(bool& out) noexcept {
  std::uint8_t raw = 0;
  if (!u8(raw)) {
    return false;
  }
  if (raw > 1) {
    return false;
  }
  out = raw == 1;
  return true;
}

bool Decoder::id128(Id128& out) noexcept {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  if (!u64(hi) || !u64(lo)) {
    return false;
  }
  out = Id128{hi, lo};
  return true;
}

bool Decoder::bytes(std::size_t count, std::span<const std::byte>& out) noexcept {
  return take(count, out);
}

bool Decoder::bytes(std::span<const std::byte>& out, std::uint32_t max_bytes) noexcept {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  if (length > max_bytes) {
    return false;
  }
  return take(length, out);
}

bool Decoder::string(std::string& out, std::uint32_t max_bytes) {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  if (length > max_bytes) {
    return false;
  }
  std::span<const std::byte> raw;
  if (!take(length, raw)) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(raw.data()), raw.size());
  return true;
}

bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return false;
  }
  out = a + b;
  return true;
}

bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return false;
  }
  out = a * b;
  return true;
}

} // namespace cable_registry
