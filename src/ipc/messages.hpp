// Cable Attachment Registry — request and response payload encoding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal header shared by the server and the client so both sides agree on
// one encoding. Every decoder takes the Limits that bound it.

#ifndef CABLE_REGISTRY_IPC_MESSAGES_HPP
#define CABLE_REGISTRY_IPC_MESSAGES_HPP

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "cable_registry/codec.hpp"
#include "cable_registry/evidence.hpp"
#include "cable_registry/graph.hpp"
#include "cable_registry/limits.hpp"
#include "cable_registry/registry.hpp"
#include "cable_registry/serialization.hpp"

namespace cable_registry::ipc {

inline void encode_policy(Encoder& out, ValidationPolicy policy) {
  out.u8(static_cast<std::uint8_t>(policy));
}

inline bool decode_policy(Decoder& in, ValidationPolicy& out) {
  std::uint8_t raw = 0;
  if (!in.u8(raw) || raw > static_cast<std::uint8_t>(ValidationPolicy::LiveOnly)) {
    return false;
  }
  out = static_cast<ValidationPolicy>(raw);
  return true;
}

inline void encode_object_id(Encoder& out, const ObjectId& value) {
  out.id128(value.value());
}

inline void encode_endpoint_id(Encoder& out, const EndpointId& value) {
  out.id128(value.value());
}

inline void encode_port_ref(Encoder& out, const PortRef& value) {
  encode_endpoint_id(out, value.endpoint);
  out.u32(value.index.value);
}

inline bool decode_port_ref(Decoder& in, PortRef& out) {
  PortRef staged;
  if (!in.id128(staged.endpoint) || !in.u32(staged.index.value)) {
    return false;
  }
  out = staged;
  return true;
}

inline void encode_source_key(Encoder& out, const SourceIncarnationKey& value) {
  out.id128(value.source.value());
  out.u64(value.incarnation.value);
}

inline bool decode_source_key(Decoder& in, SourceIncarnationKey& out) {
  SourceIncarnationKey staged;
  if (!in.id128(staged.source) || !in.u64(staged.incarnation.value)) {
    return false;
  }
  out = staged;
  return true;
}

inline void encode_digest(Encoder& out, const Digest256& value) {
  out.bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(value.bytes.data()),
                                       value.bytes.size()));
}

inline void encode_failure(Encoder& out, const Error& error, const Limits& limits) {
  out.u16(static_cast<std::uint16_t>(error.code()));
  out.string(error.message(), limits.max_string_bytes);
}

inline bool decode_failure(Decoder& in, const Limits& limits, Error& out) {
  std::uint16_t code = 0;
  std::string message;
  if (!in.u16(code) || !in.string(message, limits.max_string_bytes)) {
    return false;
  }
  if (code > static_cast<std::uint16_t>(ErrorCode::PermissionDenied)) {
    return false;
  }
  out = Error(static_cast<ErrorCode>(code), std::move(message));
  return true;
}

// -- request payloads -------------------------------------------------------

inline std::vector<std::byte> encode_open_session_request(const SourceDescriptor& descriptor,
                                                          Incarnation incarnation,
                                                          const Limits& limits) {
  Encoder encoder;
  encode_source_descriptor(encoder, descriptor, limits);
  encoder.u64(incarnation.value);
  return encoder.take();
}

inline bool decode_open_session_request(std::span<const std::byte> data,
                                        const Limits& limits,
                                        SourceDescriptor& descriptor,
                                        Incarnation& incarnation) {
  Decoder decoder(data);
  SourceDescriptor staged_descriptor;
  Incarnation staged_incarnation;
  if (!decode_source_descriptor(decoder, limits, staged_descriptor) ||
      !decoder.u64(staged_incarnation.value) || !decoder.done()) {
    return false;
  }
  descriptor = std::move(staged_descriptor);
  incarnation = staged_incarnation;
  return true;
}

inline bool decode_close_session_request(std::span<const std::byte> data, SourceIncarnationKey& key) {
  Decoder decoder(data);
  if (!decode_source_key(decoder, key) || !decoder.done()) {
    return false;
  }
  return true;
}

inline std::vector<std::byte> encode_publish_request(const Evidence& evidence, const Limits& limits) {
  return encode_evidence(evidence, limits);
}

inline std::vector<std::byte> encode_publish_batch_request(std::span<const Evidence> batch, const Limits& limits) {
  Encoder encoder;
  encoder.u32(static_cast<std::uint32_t>(batch.size()));
  for (const Evidence& evidence : batch) {
    encode_evidence(encoder, evidence, limits);
  }
  return encoder.take();
}

inline bool decode_publish_batch_request(std::span<const std::byte> data,
                                         const Limits& limits,
                                         std::vector<Evidence>& out) {
  Decoder decoder(data);
  std::uint32_t count = 0;
  if (!decoder.u32(count) || count > limits.max_evidence_batch) {
    return false;
  }
  std::vector<Evidence> staged;
  staged.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    Evidence evidence;
    if (!decode_evidence(decoder, limits, evidence)) {
      return false;
    }
    staged.push_back(std::move(evidence));
  }
  if (!decoder.done()) {
    return false;
  }
  out = std::move(staged);
  return true;
}

inline std::vector<std::byte> encode_port_query(PortRef port, ValidationPolicy policy) {
  Encoder encoder;
  encode_port_ref(encoder, port);
  encode_policy(encoder, policy);
  return encoder.take();
}

inline bool decode_port_query(std::span<const std::byte> data, PortRef& port, ValidationPolicy& policy) {
  Decoder decoder(data);
  if (!decode_port_ref(decoder, port) || !decode_policy(decoder, policy) || !decoder.done()) {
    return false;
  }
  return true;
}

inline std::vector<std::byte> encode_endpoint_query(EndpointId endpoint, bool include_ports, ValidationPolicy policy) {
  Encoder encoder;
  encode_endpoint_id(encoder, endpoint);
  encoder.boolean(include_ports);
  encode_policy(encoder, policy);
  return encoder.take();
}

inline bool decode_endpoint_query(std::span<const std::byte> data,
                                  EndpointId& endpoint,
                                  bool& include_ports,
                                  ValidationPolicy& policy) {
  Decoder decoder(data);
  if (!decoder.id128(endpoint) || !decoder.boolean(include_ports) || !decode_policy(decoder, policy) ||
      !decoder.done()) {
    return false;
  }
  return true;
}

inline std::vector<std::byte> encode_object_query(ObjectId object, const QueryOptions& options) {
  Encoder encoder;
  encode_object_id(encoder, object);
  encode_query_options(encoder, options);
  return encoder.take();
}

inline bool decode_object_query(std::span<const std::byte> data, ObjectId& object, QueryOptions& options) {
  Decoder decoder(data);
  if (!decoder.id128(object) || !decode_query_options(decoder, options) || !decoder.done()) {
    return false;
  }
  return true;
}

inline std::vector<std::byte> encode_history_query(ObjectId object, std::uint32_t max_events) {
  Encoder encoder;
  encode_object_id(encoder, object);
  encoder.u32(max_events);
  return encoder.take();
}

inline bool decode_history_query(std::span<const std::byte> data, ObjectId& object, std::uint32_t& max_events) {
  Decoder decoder(data);
  if (!decoder.id128(object) || !decoder.u32(max_events) || !decoder.done()) {
    return false;
  }
  return true;
}

inline std::vector<std::byte> encode_inspect_query(std::uint32_t max_entries) {
  Encoder encoder;
  encoder.u32(max_entries);
  return encoder.take();
}

inline bool decode_inspect_query(std::span<const std::byte> data, std::uint32_t& max_entries) {
  Decoder decoder(data);
  if (!decoder.u32(max_entries) || !decoder.done()) {
    return false;
  }
  return true;
}

// -- response payloads ------------------------------------------------------

inline bool decode_port_response(std::span<const std::byte> data, const Limits& limits, PortView& out) {
  Decoder decoder(data);
  if (!decode_port_view(decoder, limits, out) || !decoder.done()) {
    return false;
  }
  return true;
}

inline bool decode_endpoint_response(std::span<const std::byte> data, const Limits& limits, EndpointView& out) {
  Decoder decoder(data);
  if (!decode_endpoint_view(decoder, limits, out) || !decoder.done()) {
    return false;
  }
  return true;
}

inline bool decode_object_response(std::span<const std::byte> data, const Limits& limits, ObjectView& out) {
  Decoder decoder(data);
  if (!decode_object_view(decoder, limits, out) || !decoder.done()) {
    return false;
  }
  return true;
}

inline bool decode_history_response(std::span<const std::byte> data, const Limits& limits, ObjectHistory& out) {
  Decoder decoder(data);
  if (!decode_object_history(decoder, limits, out) || !decoder.done()) {
    return false;
  }
  return true;
}

inline bool decode_inspect_response(std::span<const std::byte> data, const Limits& limits, InspectionView& out) {
  Decoder decoder(data);
  if (!decode_inspection_view(decoder, limits, out) || !decoder.done()) {
    return false;
  }
  return true;
}

inline bool decode_stats_response(std::span<const std::byte> data, const Limits& limits, RegistryStats& out) {
  Decoder decoder(data);
  if (!decode_registry_stats(decoder, limits, out) || !decoder.done()) {
    return false;
  }
  return true;
}

inline bool decode_digest_response(std::span<const std::byte> data, Digest256& out) {
  Decoder decoder(data);
  std::span<const std::byte> raw;
  if (!decoder.bytes(raw, static_cast<std::uint32_t>(out.bytes.size())) || !decoder.done()) {
    return false;
  }
  if (raw.size() != out.bytes.size()) {
    return false;
  }
  std::memcpy(out.bytes.data(), raw.data(), out.bytes.size());
  return true;
}

} // namespace cable_registry::ipc

#endif // CABLE_REGISTRY_IPC_MESSAGES_HPP
