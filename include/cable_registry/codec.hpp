// Cable Attachment Registry — bounded binary encoding primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_CODEC_HPP
#define CABLE_REGISTRY_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cable_registry/export.hpp"
#include "cable_registry/ids.hpp"

namespace cable_registry {

/// Little-endian, length-prefixed, self-delimiting writer.
///
/// Every length is written as a 32 bit count of bytes. The encoder enforces a
/// hard ceiling on the buffer it will grow to, so a caller that miscomputes a
/// length fails the encode instead of exhausting memory.
class CABLE_REGISTRY_API Encoder {
 public:
  /// Ceiling on one encoded buffer.
  static constexpr std::size_t kMaxEncodedBytes = 8u << 20;

  Encoder() = default;

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);
  void id128(const Id128& value);
  void bytes(std::span<const std::byte> value);
  /// Writes a 32 bit byte length followed by the bytes. Refuses text longer
  /// than @p max_bytes.
  void string(std::string_view value, std::uint32_t max_bytes);

  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] std::span<const std::byte> view() const noexcept { return buffer_; }
  /// The encoded bytes. Empty when ok() is false.
  [[nodiscard]] std::vector<std::byte> take() {
    if (failed_) {
      return {};
    }
    return std::move(buffer_);
  }
  void clear() noexcept {
    buffer_.clear();
    failed_ = false;
  }

  /// False once a write could not be represented: a string longer than the
  /// caller's ceiling, or a buffer that would exceed kMaxEncodedBytes. Once
  /// false every later write is a no-op, so a partially written field is never
  /// silently emitted as if it were complete.
  [[nodiscard]] bool ok() const noexcept { return !failed_; }

 private:
  void reserve_for(std::size_t extra);
  void fail() noexcept {
    failed_ = true;
    buffer_.clear();
  }

  std::vector<std::byte> buffer_;
  bool failed_ = false;
};

/// Reader for the encoding produced by Encoder. Every accessor returns false
/// and leaves the output untouched when the buffer is exhausted or when a
/// declared length exceeds the ceiling handed to the accessor.
class CABLE_REGISTRY_API Decoder {
 public:
  Decoder() = default;
  explicit Decoder(std::span<const std::byte> data) noexcept : data_(data) {}

  [[nodiscard]] bool u8(std::uint8_t& out) noexcept;
  [[nodiscard]] bool u16(std::uint16_t& out) noexcept;
  [[nodiscard]] bool u32(std::uint32_t& out) noexcept;
  [[nodiscard]] bool u64(std::uint64_t& out) noexcept;
  [[nodiscard]] bool i64(std::int64_t& out) noexcept;
  [[nodiscard]] bool boolean(bool& out) noexcept;
  [[nodiscard]] bool id128(Id128& out) noexcept;
  /// Strongly typed overload. Distinct identity types never decode into one
  /// another.
  template <class Tag>
  [[nodiscard]] bool id128(StrongId<Tag>& out) noexcept {
    Id128 raw;
    if (!id128(raw)) {
      return false;
    }
    out = StrongId<Tag>(raw);
    return true;
  }
  /// Reads exactly @p count raw bytes.
  [[nodiscard]] bool bytes(std::size_t count, std::span<const std::byte>& out) noexcept;
  /// Reads the 32 bit length prefix Encoder::bytes() wrote, then that many
  /// bytes. Refuses a length above @p max_bytes before reading.
  [[nodiscard]] bool bytes(std::span<const std::byte>& out, std::uint32_t max_bytes) noexcept;
  [[nodiscard]] bool string(std::string& out, std::uint32_t max_bytes);

  [[nodiscard]] bool done() const noexcept { return offset_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - offset_; }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  void reset(std::span<const std::byte> data) noexcept {
    data_ = data;
    offset_ = 0;
  }

 private:
  [[nodiscard]] bool take(std::size_t count, std::span<const std::byte>& out) noexcept;

  std::span<const std::byte> data_{};
  std::size_t offset_ = 0;
};

/// Checked arithmetic helpers used wherever a size is derived from input.
[[nodiscard]] CABLE_REGISTRY_API bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;
[[nodiscard]] CABLE_REGISTRY_API bool checked_mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept;

} // namespace cable_registry

#endif // CABLE_REGISTRY_CODEC_HPP
