// Cable Attachment Registry — SHA-256 and CRC32C.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Both primitives are implemented here rather than pulled in as a dependency:
// the runtime is deliberately dependency free. SHA-256 is FIPS 180-4 and
// CRC-32C is the Castagnoli polynomial in reflected form. Neither is an
// authenticity mechanism; they detect accidental corruption and give the
// canonical encodings a stable identity.

#include "cable_registry/digest.hpp"

#include <array>
#include <cstring>

namespace cable_registry {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::array<std::uint32_t, 8> kSha256InitialState = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t crc = index;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    table[index] = crc;
  }
  return table;
}

constexpr auto kCrc32cTable = make_crc32c_table();

} // namespace

Sha256::Sha256() noexcept {
  reset();
}

void Sha256::reset() noexcept {
  state_ = kSha256InitialState;
  buffer_.fill(0);
  total_bytes_ = 0;
  buffer_used_ = 0;
  finalized_ = false;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                      (static_cast<std::uint32_t>(block[(index * 4) + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[(index * 4) + 2]) << 8) |
                      (static_cast<std::uint32_t>(block[(index * 4) + 3]));
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotate_right(schedule[index - 15], 7) ^ rotate_right(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3);
    const std::uint32_t s1 = rotate_right(schedule[index - 2], 17) ^ rotate_right(schedule[index - 2], 19) ^
                             (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t choose = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + s1 + choose + kSha256RoundConstants[index] + schedule[index];
    const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(std::span<const std::byte> data) noexcept {
  if (finalized_ || data.empty()) {
    return;
  }
  total_bytes_ += static_cast<std::uint64_t>(data.size());
  const auto* cursor = reinterpret_cast<const std::uint8_t*>(data.data());
  std::size_t remaining = data.size();

  if (buffer_used_ != 0) {
    const std::size_t want = 64 - buffer_used_;
    const std::size_t take = remaining < want ? remaining : want;
    std::memcpy(buffer_.data() + buffer_used_, cursor, take);
    buffer_used_ += take;
    cursor += take;
    remaining -= take;
    if (buffer_used_ == 64) {
      compress(buffer_.data());
      buffer_used_ = 0;
    }
  }

  while (remaining >= 64) {
    compress(cursor);
    cursor += 64;
    remaining -= 64;
  }

  if (remaining != 0) {
    std::memcpy(buffer_.data(), cursor, remaining);
    buffer_used_ = remaining;
  }
}

void Sha256::update(std::string_view text) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

Digest256 Sha256::finish() noexcept {
  if (!finalized_) {
    const std::uint64_t bit_length = total_bytes_ * 8u;
    // buffer_used_ is always strictly less than 64 here because update()
    // compresses as soon as the buffer fills.
    buffer_[buffer_used_] = 0x80u;
    ++buffer_used_;
    if (buffer_used_ > 56) {
      while (buffer_used_ < 64) {
        buffer_[buffer_used_] = 0;
        ++buffer_used_;
      }
      compress(buffer_.data());
      buffer_used_ = 0;
    }
    while (buffer_used_ < 56) {
      buffer_[buffer_used_] = 0;
      ++buffer_used_;
    }
    for (std::size_t index = 0; index < 8; ++index) {
      buffer_[56 + index] = static_cast<std::uint8_t>((bit_length >> (56 - (index * 8))) & 0xFFu);
    }
    compress(buffer_.data());
    buffer_used_ = 0;
    finalized_ = true;
  }

  Digest256 digest{};
  for (std::size_t index = 0; index < 8; ++index) {
    digest.bytes[index * 4] = static_cast<std::uint8_t>((state_[index] >> 24) & 0xFFu);
    digest.bytes[(index * 4) + 1] = static_cast<std::uint8_t>((state_[index] >> 16) & 0xFFu);
    digest.bytes[(index * 4) + 2] = static_cast<std::uint8_t>((state_[index] >> 8) & 0xFFu);
    digest.bytes[(index * 4) + 3] = static_cast<std::uint8_t>(state_[index] & 0xFFu);
  }
  return digest;
}

Digest256 sha256(std::span<const std::byte> data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return hasher.finish();
}

Digest256 sha256(std::string_view text) noexcept {
  return sha256(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::string to_hex(const Digest256& value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text(64, '0');
  for (std::size_t index = 0; index < value.bytes.size(); ++index) {
    text[index * 2] = kDigits[(value.bytes[index] >> 4) & 0xFu];
    text[(index * 2) + 1] = kDigits[value.bytes[index] & 0xFu];
  }
  return text;
}

void Crc32c::update(std::span<const std::byte> data) noexcept {
  std::uint32_t state = state_;
  for (const std::byte raw : data) {
    const auto index = static_cast<std::uint8_t>((state ^ static_cast<std::uint32_t>(raw)) & 0xFFu);
    state = kCrc32cTable[index] ^ (state >> 8);
  }
  state_ = state;
}

void Crc32c::update(std::string_view text) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  Crc32c calculator;
  calculator.update(data);
  return calculator.finish();
}

std::uint32_t crc32c(std::string_view text) noexcept {
  return crc32c(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

} // namespace cable_registry
