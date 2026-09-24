// Cable Attachment Registry — SHA-256 and CRC32C primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_DIGEST_HPP
#define CABLE_REGISTRY_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "cable_registry/export.hpp"

namespace cable_registry {

/// A 256 bit digest.
struct Digest256 {
  std::array<std::uint8_t, 32> bytes{};

  friend constexpr bool operator==(const Digest256&, const Digest256&) noexcept = default;
  friend constexpr auto operator<=>(const Digest256&, const Digest256&) noexcept = default;

  [[nodiscard]] constexpr bool is_zero() const noexcept {
    for (std::uint8_t byte : bytes) {
      if (byte != 0) {
        return false;
      }
    }
    return true;
  }
};

CABLE_REGISTRY_API std::string to_hex(const Digest256& value);

/// Incremental SHA-256.
class CABLE_REGISTRY_API Sha256 {
 public:
  Sha256() noexcept;

  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view text) noexcept;

  /// Finalizes the digest. The object is left in the finalized state; call
  /// reset() to hash again.
  [[nodiscard]] Digest256 finish() noexcept;
  void reset() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::uint64_t total_bytes_ = 0;
  std::size_t buffer_used_ = 0;
  bool finalized_ = false;
};

/// One-shot SHA-256.
CABLE_REGISTRY_API Digest256 sha256(std::span<const std::byte> data) noexcept;
CABLE_REGISTRY_API Digest256 sha256(std::string_view text) noexcept;

/// CRC-32C (Castagnoli, reflected, polynomial 0x1EDC6F41). This is an
/// accidental-corruption check, not an authenticity mechanism.
CABLE_REGISTRY_API std::uint32_t crc32c(std::span<const std::byte> data) noexcept;
CABLE_REGISTRY_API std::uint32_t crc32c(std::string_view text) noexcept;

/// Incremental CRC-32C.
class CABLE_REGISTRY_API Crc32c {
 public:
  void update(std::span<const std::byte> data) noexcept;
  void update(std::string_view text) noexcept;
  [[nodiscard]] std::uint32_t finish() const noexcept { return state_ ^ 0xFFFFFFFFu; }
  void reset() noexcept { state_ = 0xFFFFFFFFu; }

 private:
  std::uint32_t state_ = 0xFFFFFFFFu;
};

} // namespace cable_registry

#endif // CABLE_REGISTRY_DIGEST_HPP
