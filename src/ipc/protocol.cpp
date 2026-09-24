// Cable Attachment Registry — framed publisher/registry transport protocol.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Frame layout (28 byte header, little endian):
//
//   magic            u32   kFrameMagic
//   protocol version u16   kProtocolVersion
//   message type     u16
//   payload bytes    u32
//   request id       u64
//   payload crc32c   u32
//   header crc32c    u32   over the preceding 24 bytes
//
// The header checksum is what lets a reader that has lost framing detect it
// and close the connection instead of guessing at a length. The payload
// checksum catches corruption inside one message.

#include "cable_registry/ipc/protocol.hpp"

#include <cstring>
#include <string>
#include <utility>

#include "cable_registry/codec.hpp"
#include "cable_registry/digest.hpp"
#include "cable_registry/version.hpp"
#include "ipc/messages.hpp"

namespace cable_registry::ipc {
namespace {

constexpr std::size_t kHeaderChecksumOffset = 24;

void store_u16(std::span<std::byte> out, std::size_t offset, std::uint16_t value) noexcept {
  for (int index = 0; index < 2; ++index) {
    out[offset + static_cast<std::size_t>(index)] = static_cast<std::byte>((value >> (index * 8)) & 0xFFu);
  }
}

void store_u32(std::span<std::byte> out, std::size_t offset, std::uint32_t value) noexcept {
  for (int index = 0; index < 4; ++index) {
    out[offset + static_cast<std::size_t>(index)] = static_cast<std::byte>((value >> (index * 8)) & 0xFFu);
  }
}

void store_u64(std::span<std::byte> out, std::size_t offset, std::uint64_t value) noexcept {
  for (int index = 0; index < 8; ++index) {
    out[offset + static_cast<std::size_t>(index)] = static_cast<std::byte>((value >> (index * 8)) & 0xFFu);
  }
}

std::uint16_t load_u16(std::span<const std::byte> in, std::size_t offset) noexcept {
  std::uint16_t value = 0;
  for (int index = 0; index < 2; ++index) {
    value |= static_cast<std::uint16_t>(in[offset + static_cast<std::size_t>(index)]) << (index * 8);
  }
  return value;
}

std::uint32_t load_u32(std::span<const std::byte> in, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(in[offset + static_cast<std::size_t>(index)]) << (index * 8);
  }
  return value;
}

std::uint64_t load_u64(std::span<const std::byte> in, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(in[offset + static_cast<std::size_t>(index)]) << (index * 8);
  }
  return value;
}

bool known_type(std::uint16_t raw) noexcept {
  switch (static_cast<MessageType>(raw)) {
    case MessageType::Hello:
    case MessageType::HelloAck:
    case MessageType::OpenSession:
    case MessageType::OpenSessionAck:
    case MessageType::CloseSession:
    case MessageType::CloseSessionAck:
    case MessageType::Publish:
    case MessageType::PublishAck:
    case MessageType::PublishBatch:
    case MessageType::PublishBatchAck:
    case MessageType::QueryPort:
    case MessageType::QueryPortAck:
    case MessageType::QueryEndpoint:
    case MessageType::QueryEndpointAck:
    case MessageType::QueryObject:
    case MessageType::QueryObjectAck:
    case MessageType::QueryHistory:
    case MessageType::QueryHistoryAck:
    case MessageType::Inspect:
    case MessageType::InspectAck:
    case MessageType::Snapshot:
    case MessageType::SnapshotAck:
    case MessageType::Digest:
    case MessageType::DigestAck:
    case MessageType::Stats:
    case MessageType::StatsAck:
    case MessageType::Shutdown:
    case MessageType::ShutdownAck:
    case MessageType::Failure:
      return true;
  }
  return false;
}

} // namespace

const char* to_string(MessageType value) noexcept {
  switch (value) {
    case MessageType::Hello:
      return "hello";
    case MessageType::HelloAck:
      return "hello-ack";
    case MessageType::OpenSession:
      return "open-session";
    case MessageType::OpenSessionAck:
      return "open-session-ack";
    case MessageType::CloseSession:
      return "close-session";
    case MessageType::CloseSessionAck:
      return "close-session-ack";
    case MessageType::Publish:
      return "publish";
    case MessageType::PublishAck:
      return "publish-ack";
    case MessageType::PublishBatch:
      return "publish-batch";
    case MessageType::PublishBatchAck:
      return "publish-batch-ack";
    case MessageType::QueryPort:
      return "query-port";
    case MessageType::QueryPortAck:
      return "query-port-ack";
    case MessageType::QueryEndpoint:
      return "query-endpoint";
    case MessageType::QueryEndpointAck:
      return "query-endpoint-ack";
    case MessageType::QueryObject:
      return "query-object";
    case MessageType::QueryObjectAck:
      return "query-object-ack";
    case MessageType::QueryHistory:
      return "query-history";
    case MessageType::QueryHistoryAck:
      return "query-history-ack";
    case MessageType::Inspect:
      return "inspect";
    case MessageType::InspectAck:
      return "inspect-ack";
    case MessageType::Snapshot:
      return "snapshot";
    case MessageType::SnapshotAck:
      return "snapshot-ack";
    case MessageType::Digest:
      return "digest";
    case MessageType::DigestAck:
      return "digest-ack";
    case MessageType::Stats:
      return "stats";
    case MessageType::StatsAck:
      return "stats-ack";
    case MessageType::Shutdown:
      return "shutdown";
    case MessageType::ShutdownAck:
      return "shutdown-ack";
    case MessageType::Failure:
      return "failure";
  }
  return "unknown";
}

std::vector<std::byte> encode_frame(MessageType type,
                                    std::uint64_t request_id,
                                    std::span<const std::byte> payload,
                                    std::uint32_t max_payload_bytes) {
  if (payload.size() > max_payload_bytes) {
    return {};
  }
  std::vector<std::byte> frame(kFrameHeaderBytes + payload.size());
  auto view = std::span<std::byte>(frame.data(), frame.size());
  store_u32(view, 0, kFrameMagic);
  store_u16(view, 4, static_cast<std::uint16_t>(kProtocolVersion));
  store_u16(view, 6, static_cast<std::uint16_t>(type));
  store_u32(view, 8, static_cast<std::uint32_t>(payload.size()));
  store_u64(view, 12, request_id);
  store_u32(view, 20, crc32c(payload));
  store_u32(view, kHeaderChecksumOffset, crc32c(std::span<const std::byte>(view.data(), kHeaderChecksumOffset)));
  if (!payload.empty()) {
    std::memcpy(frame.data() + kFrameHeaderBytes, payload.data(), payload.size());
  }
  return frame;
}

Outcome<void> decode_frame_header(std::span<const std::byte> header,
                                  std::uint32_t max_payload_bytes,
                                  MessageType& type,
                                  std::uint64_t& request_id,
                                  std::uint32_t& payload_bytes) {
  if (header.size() != kFrameHeaderBytes) {
    return make_error(ErrorCode::ProtocolViolation, "the frame header is not the expected size");
  }
  if (load_u32(header, 0) != kFrameMagic) {
    return make_error(ErrorCode::ProtocolViolation, "the frame magic does not match");
  }
  if (load_u32(header, kHeaderChecksumOffset) !=
      crc32c(std::span<const std::byte>(header.data(), kHeaderChecksumOffset))) {
    return make_error(ErrorCode::ProtocolViolation, "the frame header checksum does not match");
  }
  const std::uint16_t version = load_u16(header, 4);
  if (version != static_cast<std::uint16_t>(kProtocolVersion)) {
    return make_error(ErrorCode::UnsupportedVersion,
                      "protocol version " + std::to_string(version) + " is not supported");
  }
  const std::uint16_t raw_type = load_u16(header, 6);
  if (!known_type(raw_type)) {
    return make_error(ErrorCode::ProtocolViolation,
                      "message type " + std::to_string(raw_type) + " is not recognised");
  }
  payload_bytes = load_u32(header, 8);
  if (payload_bytes > max_payload_bytes) {
    return make_error(ErrorCode::CapacityExceeded,
                      "frame payload of " + std::to_string(payload_bytes) + " bytes exceeds the " +
                          std::to_string(max_payload_bytes) + " byte limit");
  }
  type = static_cast<MessageType>(raw_type);
  request_id = load_u64(header, 12);
  return Outcome<void>();
}

Outcome<Frame> decode_frame(std::span<const std::byte> data, std::uint32_t max_payload_bytes) {
  if (data.size() < kFrameHeaderBytes) {
    return make_error<Frame>(ErrorCode::ProtocolViolation, "the frame is shorter than its header");
  }
  MessageType type = MessageType::Hello;
  std::uint64_t request_id = 0;
  std::uint32_t payload_bytes = 0;
  Outcome<void> header = decode_frame_header(data.first(kFrameHeaderBytes), max_payload_bytes, type,
                                             request_id, payload_bytes);
  if (!header.has_value()) {
    return make_error<Frame>(header.error());
  }
  if (data.size() != kFrameHeaderBytes + payload_bytes) {
    return make_error<Frame>(ErrorCode::ProtocolViolation, "the frame length does not match its header");
  }
  const std::span<const std::byte> payload = data.subspan(kFrameHeaderBytes, payload_bytes);
  if (crc32c(payload) != load_u32(data, 20)) {
    return make_error<Frame>(ErrorCode::ProtocolViolation, "the frame payload checksum does not match");
  }
  Frame frame;
  frame.type = type;
  frame.request_id = request_id;
  frame.payload.assign(payload.begin(), payload.end());
  return frame;
}

std::vector<std::byte> encode_hello(const Hello& value, const Limits& limits) {
  Encoder encoder;
  encoder.u32(value.protocol_version);
  encoder.u32(value.library_version);
  encoder.id128(value.registry.value());
  encoder.u32(value.max_payload_bytes);
  encoder.boolean(value.shutdown_allowed);
  encoder.string(value.identity, limits.max_string_bytes);
  return encoder.take();
}

bool decode_hello(std::span<const std::byte> data, const Limits& limits, Hello& out) {
  Decoder decoder(data);
  Hello staged;
  if (!decoder.u32(staged.protocol_version) || !decoder.u32(staged.library_version) ||
      !decoder.id128(staged.registry) || !decoder.u32(staged.max_payload_bytes) ||
      !decoder.boolean(staged.shutdown_allowed) || !decoder.string(staged.identity, limits.max_string_bytes)) {
    return false;
  }
  if (!decoder.done()) {
    return false;
  }
  out = std::move(staged);
  return true;
}

} // namespace cable_registry::ipc
