// Cable Attachment Registry — framed publisher/registry transport protocol.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_IPC_PROTOCOL_HPP
#define CABLE_REGISTRY_IPC_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cable_registry/error.hpp"
#include "cable_registry/export.hpp"
#include "cable_registry/graph.hpp"
#include "cable_registry/limits.hpp"
#include "cable_registry/registry.hpp"
#include "cable_registry/snapshot.hpp"

namespace cable_registry::ipc {

/// Magic that starts every frame header. A reader that sees anything else has
/// lost framing and must close the connection rather than guess.
inline constexpr std::uint32_t kFrameMagic = 0x31424143u; // "CAB1"
/// Bytes of a frame header on the wire.
inline constexpr std::size_t kFrameHeaderBytes = 28;

enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  OpenSession = 10,
  OpenSessionAck = 11,
  CloseSession = 12,
  CloseSessionAck = 13,
  Publish = 20,
  PublishAck = 21,
  PublishBatch = 22,
  PublishBatchAck = 23,
  QueryPort = 30,
  QueryPortAck = 31,
  QueryEndpoint = 32,
  QueryEndpointAck = 33,
  QueryObject = 34,
  QueryObjectAck = 35,
  QueryHistory = 36,
  QueryHistoryAck = 37,
  Inspect = 38,
  InspectAck = 39,
  Snapshot = 50,
  SnapshotAck = 51,
  Digest = 52,
  DigestAck = 53,
  Stats = 54,
  StatsAck = 55,
  Shutdown = 60,
  ShutdownAck = 61,
  /// Any request that failed. The payload is the rendered error.
  Failure = 70,
};

CABLE_REGISTRY_API const char* to_string(MessageType value) noexcept;

/// A decoded frame.
struct Frame {
  MessageType type = MessageType::Hello;
  std::uint64_t request_id = 0;
  std::vector<std::byte> payload;
};

/// The greeting a registry sends when a connection opens.
struct Hello {
  std::uint32_t protocol_version = 0;
  std::uint32_t library_version = 0;
  RegistryId registry{};
  std::uint32_t max_payload_bytes = 0;
  bool shutdown_allowed = false;
  std::string identity;
};

CABLE_REGISTRY_API std::vector<std::byte> encode_frame(MessageType type,
                                                       std::uint64_t request_id,
                                                       std::span<const std::byte> payload,
                                                       std::uint32_t max_payload_bytes);

/// Decodes a complete frame, header and payload. @p data must hold exactly one
/// frame; a caller reads kFrameHeaderBytes first to learn the payload length,
/// checks it against its own bound, and then reads the remainder.
CABLE_REGISTRY_API Outcome<Frame> decode_frame(std::span<const std::byte> data, std::uint32_t max_payload_bytes);

/// Decodes and validates the header. On success @p payload_bytes is the size
/// of the payload that must follow.
CABLE_REGISTRY_API Outcome<void> decode_frame_header(std::span<const std::byte> header,
                                                     std::uint32_t max_payload_bytes,
                                                     MessageType& type,
                                                     std::uint64_t& request_id,
                                                     std::uint32_t& payload_bytes);

CABLE_REGISTRY_API std::vector<std::byte> encode_hello(const Hello& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_hello(std::span<const std::byte> data, const Limits& limits, Hello& out);

} // namespace cable_registry::ipc

#endif // CABLE_REGISTRY_IPC_PROTOCOL_HPP
