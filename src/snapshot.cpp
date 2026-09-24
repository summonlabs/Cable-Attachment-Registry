// Cable Attachment Registry — canonical snapshot encoding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The canonical encoding is the snapshot's identity. It contains nothing that
// varies with arrival order, wall clock, or registry lifetime: no receive
// timestamps, no validation state, no sequence numbers. Two registries holding
// the same effective claims encode byte-identical images. Collections are
// sorted here, so a snapshot assembled by hand is canonicalised as well.

#include "cable_registry/snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>

#include "cable_registry/codec.hpp"
#include "cable_registry/serialization.hpp"

namespace cable_registry {
namespace {

template <class T, class DecodeFn>
bool decode_vector(Decoder& in, std::uint32_t max_count, std::vector<T>& out, DecodeFn decode_element) {
  std::uint32_t count = 0;
  if (!in.u32(count)) {
    return false;
  }
  if (count > max_count) {
    return false;
  }
  std::vector<T> staged;
  staged.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    T element{};
    if (!decode_element(element)) {
      return false;
    }
    staged.push_back(std::move(element));
  }
  out = std::move(staged);
  return true;
}

bool claim_less(const SnapshotClaim& left, const SnapshotClaim& right) {
  if (left.source != right.source) {
    return left.source < right.source;
  }
  if (left.incarnation != right.incarnation) {
    return left.incarnation < right.incarnation;
  }
  if (left.generation != right.generation) {
    return left.generation < right.generation;
  }
  return left.evidence < right.evidence;
}

void encode_claim(Encoder& out, const SnapshotClaim& value) {
  out.id128(value.source.value());
  out.u64(value.incarnation.value);
  out.u64(value.generation.value);
  out.id128(value.evidence.value());
  out.i64(value.observed_at.unix_nanos);
  out.u8(static_cast<std::uint8_t>(value.provenance));
  out.u8(static_cast<std::uint8_t>(value.authority.authority_class));
  out.u32(value.authority.rank);
}

bool decode_claim(Decoder& in, SnapshotClaim& out) {
  SnapshotClaim staged;
  std::uint8_t provenance = 0;
  std::uint8_t authority_class = 0;
  std::uint32_t rank = 0;
  if (!in.id128(staged.source) || !in.u64(staged.incarnation.value) || !in.u64(staged.generation.value) ||
      !in.id128(staged.evidence) || !in.i64(staged.observed_at.unix_nanos) || !in.u8(provenance) ||
      !in.u8(authority_class) || !in.u32(rank)) {
    return false;
  }
  if (provenance > static_cast<std::uint8_t>(ProvenanceClass::Synthetic) ||
      authority_class > static_cast<std::uint8_t>(AuthorityClass::Authoritative)) {
    return false;
  }
  staged.provenance = static_cast<ProvenanceClass>(provenance);
  staged.authority.authority_class = static_cast<AuthorityClass>(authority_class);
  staged.authority.rank = rank;
  out = staged;
  return true;
}

void encode_claims(Encoder& out, const std::vector<SnapshotClaim>& claims) {
  out.u32(static_cast<std::uint32_t>(claims.size()));
  for (const SnapshotClaim& claim : claims) {
    encode_claim(out, claim);
  }
}

bool decode_claims(Decoder& in, const Limits& limits, std::vector<SnapshotClaim>& out) {
  return decode_vector<SnapshotClaim>(in, limits.max_claim_records_per_port, out,
                                      [&](SnapshotClaim& element) { return decode_claim(in, element); });
}

void encode_attachment(Encoder& out, const SnapshotAttachment& value, const Limits& limits) {
  out.id128(value.subject.object.value());
  out.u32(value.subject.side.value);
  out.id128(value.port.endpoint.value());
  out.u32(value.port.index.value);
  out.u32(value.slot.value);
  out.u64(value.object_incarnation.value);
  out.u8(static_cast<std::uint8_t>(value.authority.authority_class));
  out.u32(value.authority.rank);
  out.boolean(value.connector_compatible);
  out.boolean(value.media_compatible);
  out.boolean(value.conflicting);
  encode_claims(out, value.claims);
  (void)limits;
}

bool decode_attachment(Decoder& in, const Limits& limits, SnapshotAttachment& out) {
  SnapshotAttachment staged;
  std::uint8_t authority_class = 0;
  if (!in.id128(staged.subject.object) || !in.u32(staged.subject.side.value) || !in.id128(staged.port.endpoint) ||
      !in.u32(staged.port.index.value) || !in.u32(staged.slot.value) ||
      !in.u64(staged.object_incarnation.value) || !in.u8(authority_class) || !in.u32(staged.authority.rank) ||
      !in.boolean(staged.connector_compatible) || !in.boolean(staged.media_compatible) ||
      !in.boolean(staged.conflicting)) {
    return false;
  }
  if (authority_class > static_cast<std::uint8_t>(AuthorityClass::Authoritative)) {
    return false;
  }
  staged.authority.authority_class = static_cast<AuthorityClass>(authority_class);
  if (!decode_claims(in, limits, staged.claims)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_conflict(Encoder& out, const SnapshotConflict& value) {
  out.u8(static_cast<std::uint8_t>(value.reason));
  out.id128(value.subject.object.value());
  out.u32(value.subject.side.value);
  out.id128(value.port.endpoint.value());
  out.u32(value.port.index.value);
  encode_claims(out, value.claims);
}

bool decode_conflict(Decoder& in, const Limits& limits, SnapshotConflict& out) {
  SnapshotConflict staged;
  std::uint8_t reason = 0;
  if (!in.u8(reason) || !in.id128(staged.subject.object) || !in.u32(staged.subject.side.value) ||
      !in.id128(staged.port.endpoint) || !in.u32(staged.port.index.value)) {
    return false;
  }
  if (reason > static_cast<std::uint8_t>(ConflictReason::IncompatibleMedia)) {
    return false;
  }
  staged.reason = static_cast<ConflictReason>(reason);
  if (!decode_claims(in, limits, staged.claims)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_port(Encoder& out, const SnapshotPort& value, const Limits& limits) {
  out.id128(value.port.endpoint.value());
  out.u32(value.port.index.value);
  out.u8(static_cast<std::uint8_t>(value.state));
  encode_port_capability(out, value.capability, limits);
  out.u32(static_cast<std::uint32_t>(value.attachments.size()));
  for (const SnapshotAttachment& attachment : value.attachments) {
    encode_attachment(out, attachment, limits);
  }
  out.u32(static_cast<std::uint32_t>(value.conflicts.size()));
  for (const SnapshotConflict& conflict : value.conflicts) {
    encode_conflict(out, conflict);
  }
}

bool decode_port(Decoder& in, const Limits& limits, SnapshotPort& out) {
  SnapshotPort staged;
  std::uint8_t state = 0;
  if (!in.id128(staged.port.endpoint) || !in.u32(staged.port.index.value) || !in.u8(state) ||
      !decode_port_capability(in, limits, staged.capability)) {
    return false;
  }
  if (state > static_cast<std::uint8_t>(PortAttachmentState::Conflicting)) {
    return false;
  }
  staged.state = static_cast<PortAttachmentState>(state);
  if (!decode_vector<SnapshotAttachment>(in,
                                         limits.max_claim_records_per_port,
                                         staged.attachments,
                                         [&](SnapshotAttachment& element) {
                                           return decode_attachment(in, limits, element);
                                         })) {
    return false;
  }
  if (!decode_vector<SnapshotConflict>(in,
                                       limits.max_claim_records_per_port,
                                       staged.conflicts,
                                       [&](SnapshotConflict& element) {
                                         return decode_conflict(in, limits, element);
                                       })) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_object(Encoder& out, const SnapshotObject& value, const Limits& limits) {
  out.id128(value.id.value());
  out.u8(static_cast<std::uint8_t>(value.kind));
  out.string(value.physical_label, limits.max_string_bytes);
  out.string(value.serial_like, limits.max_string_bytes);
  out.string(value.administrative_location, limits.max_string_bytes);
  out.u64(value.incarnation.value);
  out.u8(static_cast<std::uint8_t>(value.lifecycle));
  encode_capability(out, value.capability, limits);
  out.u32(static_cast<std::uint32_t>(value.sides.size()));
  for (const ObjectSideDescriptor& side : value.sides) {
    out.u8(static_cast<std::uint8_t>(side.connector));
    out.u8(static_cast<std::uint8_t>(side.media));
    out.u16(side.lanes.value);
  }
  out.u32(static_cast<std::uint32_t>(value.attachments.size()));
  for (const SnapshotAttachment& attachment : value.attachments) {
    encode_attachment(out, attachment, limits);
  }
}

bool decode_object(Decoder& in, const Limits& limits, SnapshotObject& out) {
  SnapshotObject staged;
  std::uint8_t kind = 0;
  std::uint8_t lifecycle = 0;
  if (!in.id128(staged.id) || !in.u8(kind) || !in.string(staged.physical_label, limits.max_string_bytes) ||
      !in.string(staged.serial_like, limits.max_string_bytes) ||
      !in.string(staged.administrative_location, limits.max_string_bytes) ||
      !in.u64(staged.incarnation.value) || !in.u8(lifecycle)) {
    return false;
  }
  if (kind > static_cast<std::uint8_t>(ObjectKind::Other) ||
      lifecycle > static_cast<std::uint8_t>(LifecycleState::Conflicting)) {
    return false;
  }
  staged.kind = static_cast<ObjectKind>(kind);
  staged.lifecycle = static_cast<LifecycleState>(lifecycle);
  if (!decode_capability(in, limits, staged.capability)) {
    return false;
  }
  if (!decode_vector<ObjectSideDescriptor>(in,
                                           limits.max_sides_per_object,
                                           staged.sides,
                                           [&](ObjectSideDescriptor& element) {
                                             std::uint8_t connector = 0;
                                             std::uint8_t media = 0;
                                             if (!in.u8(connector) || !in.u8(media) ||
                                                 !in.u16(element.lanes.value)) {
                                               return false;
                                             }
                                             if (connector > static_cast<std::uint8_t>(ConnectorClass::Other) ||
                                                 media > static_cast<std::uint8_t>(MediaClass::Other)) {
                                               return false;
                                             }
                                             element.connector = static_cast<ConnectorClass>(connector);
                                             element.media = static_cast<MediaClass>(media);
                                             return true;
                                           })) {
    return false;
  }
  if (!decode_vector<SnapshotAttachment>(in,
                                         limits.max_claim_records_per_port,
                                         staged.attachments,
                                         [&](SnapshotAttachment& element) {
                                           return decode_attachment(in, limits, element);
                                         })) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_endpoint(Encoder& out, const SnapshotEndpoint& value, const Limits& limits) {
  out.id128(value.id.value());
  out.u8(static_cast<std::uint8_t>(value.kind));
  out.string(value.name, limits.max_string_bytes);
  out.string(value.administrative_location, limits.max_string_bytes);
  out.u32(value.port_count);
  encode_port_capability(out, value.default_port_capability, limits);
}

bool decode_endpoint(Decoder& in, const Limits& limits, SnapshotEndpoint& out) {
  SnapshotEndpoint staged;
  std::uint8_t kind = 0;
  if (!in.id128(staged.id) || !in.u8(kind) || !in.string(staged.name, limits.max_string_bytes) ||
      !in.string(staged.administrative_location, limits.max_string_bytes) || !in.u32(staged.port_count)) {
    return false;
  }
  if (kind > static_cast<std::uint8_t>(EndpointKind::Other) || staged.port_count > limits.max_ports_per_endpoint) {
    return false;
  }
  staged.kind = static_cast<EndpointKind>(kind);
  if (!decode_port_capability(in, limits, staged.default_port_capability)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_summary(Encoder& out, const GraphSummary& value) {
  out.u64(value.ports_attached);
  out.u64(value.ports_empty);
  out.u64(value.ports_unknown);
  out.u64(value.ports_conflicting);
  out.u64(value.objects_total);
  out.u64(value.objects_registered);
  out.u64(value.objects_unattached);
  out.u64(value.objects_attached);
  out.u64(value.objects_quarantined);
  out.u64(value.objects_removed);
  out.u64(value.objects_retired);
  out.u64(value.edges);
  out.u64(value.edges_unvalidated);
  out.u64(value.conflicts);
  out.u64(value.claims_fenced);
  out.u64(value.claims_pending);
}

bool decode_summary(Decoder& in, GraphSummary& out) {
  GraphSummary staged;
  if (!in.u64(staged.ports_attached) || !in.u64(staged.ports_empty) || !in.u64(staged.ports_unknown) ||
      !in.u64(staged.ports_conflicting) || !in.u64(staged.objects_total) || !in.u64(staged.objects_registered) ||
      !in.u64(staged.objects_unattached) || !in.u64(staged.objects_attached) ||
      !in.u64(staged.objects_quarantined) || !in.u64(staged.objects_removed) || !in.u64(staged.objects_retired) ||
      !in.u64(staged.edges) || !in.u64(staged.edges_unvalidated) || !in.u64(staged.conflicts) ||
      !in.u64(staged.claims_fenced) || !in.u64(staged.claims_pending)) {
    return false;
  }
  out = staged;
  return true;
}

/// Sorts every collection into its canonical order.
void canonicalise(TopologySnapshot& snapshot) {
  std::sort(snapshot.objects.begin(),
            snapshot.objects.end(),
            [](const SnapshotObject& left, const SnapshotObject& right) { return left.id < right.id; });
  std::sort(snapshot.endpoints.begin(),
            snapshot.endpoints.end(),
            [](const SnapshotEndpoint& left, const SnapshotEndpoint& right) { return left.id < right.id; });
  std::sort(snapshot.ports.begin(),
            snapshot.ports.end(),
            [](const SnapshotPort& left, const SnapshotPort& right) { return left.port < right.port; });
  std::sort(snapshot.fenced_sessions.begin(),
            snapshot.fenced_sessions.end(),
            [](const SnapshotFencedSession& left, const SnapshotFencedSession& right) {
              if (left.source != right.source) {
                return left.source < right.source;
              }
              return left.incarnation < right.incarnation;
            });
  std::sort(snapshot.fenced_objects.begin(),
            snapshot.fenced_objects.end(),
            [](const SnapshotFencedObject& left, const SnapshotFencedObject& right) {
              return left.object < right.object;
            });

  for (SnapshotObject& object : snapshot.objects) {
    std::sort(object.attachments.begin(),
              object.attachments.end(),
              [](const SnapshotAttachment& left, const SnapshotAttachment& right) {
                if (left.port != right.port) {
                  return left.port < right.port;
                }
                return left.slot < right.slot;
              });
    for (SnapshotAttachment& attachment : object.attachments) {
      std::sort(attachment.claims.begin(), attachment.claims.end(), claim_less);
    }
  }

  for (SnapshotPort& port : snapshot.ports) {
    std::sort(port.attachments.begin(),
              port.attachments.end(),
              [](const SnapshotAttachment& left, const SnapshotAttachment& right) {
                if (left.subject != right.subject) {
                  return left.subject < right.subject;
                }
                return left.slot < right.slot;
              });
    std::sort(port.conflicts.begin(),
              port.conflicts.end(),
              [](const SnapshotConflict& left, const SnapshotConflict& right) {
                if (left.reason != right.reason) {
                  return left.reason < right.reason;
                }
                if (left.subject != right.subject) {
                  return left.subject < right.subject;
                }
                return left.port < right.port;
              });
    for (SnapshotAttachment& attachment : port.attachments) {
      std::sort(attachment.claims.begin(), attachment.claims.end(), claim_less);
    }
    for (SnapshotConflict& conflict : port.conflicts) {
      std::sort(conflict.claims.begin(), conflict.claims.end(), claim_less);
    }
  }
}

std::vector<std::byte> encode_snapshot_bytes(const TopologySnapshot& source,
                                             bool include_registry,
                                             const Limits& limits) {
  TopologySnapshot snapshot = source;
  canonicalise(snapshot);

  Encoder encoder;
  encoder.u32(snapshot.format_version);
  if (include_registry) {
    encoder.id128(snapshot.registry.value());
  }
  const auto* digest_bytes = reinterpret_cast<const std::byte*>(snapshot.provenance_digest.bytes.data());
  encoder.bytes(std::span<const std::byte>(digest_bytes, snapshot.provenance_digest.bytes.size()));
  encode_summary(encoder, snapshot.summary);

  encoder.u32(static_cast<std::uint32_t>(snapshot.objects.size()));
  for (const SnapshotObject& object : snapshot.objects) {
    encode_object(encoder, object, limits);
  }
  encoder.u32(static_cast<std::uint32_t>(snapshot.endpoints.size()));
  for (const SnapshotEndpoint& endpoint : snapshot.endpoints) {
    encode_endpoint(encoder, endpoint, limits);
  }
  encoder.u32(static_cast<std::uint32_t>(snapshot.ports.size()));
  for (const SnapshotPort& port : snapshot.ports) {
    encode_port(encoder, port, limits);
  }
  encoder.u32(static_cast<std::uint32_t>(snapshot.fenced_sessions.size()));
  for (const SnapshotFencedSession& session : snapshot.fenced_sessions) {
    encoder.id128(session.source.value());
    encoder.u64(session.incarnation.value);
    encoder.u64(session.fenced_by.value);
    encoder.u64(session.claims);
  }
  encoder.u32(static_cast<std::uint32_t>(snapshot.fenced_objects.size()));
  for (const SnapshotFencedObject& object : snapshot.fenced_objects) {
    encoder.id128(object.object.value());
    encoder.u64(object.current_incarnation.value);
    encoder.u64(object.fenced_claims);
  }
  return encoder.take();
}

} // namespace

std::vector<std::byte> TopologySnapshot::encode() const {
  return encode_snapshot_bytes(*this, true, Limits{});
}

Digest256 TopologySnapshot::digest() const {
  const std::vector<std::byte> bytes = encode();
  return sha256(bytes);
}

Digest256 TopologySnapshot::graph_digest() const {
  const std::vector<std::byte> bytes = encode_snapshot_bytes(*this, false, Limits{});
  return sha256(bytes);
}

void encode_topology_snapshot(Encoder& out, const TopologySnapshot& value, const Limits& limits) {
  const std::vector<std::byte> bytes = encode_snapshot_bytes(value, true, limits);
  out.bytes(bytes);
}

bool decode_topology_snapshot(Decoder& in, const Limits& limits, TopologySnapshot& out) {
  std::uint32_t length = 0;
  if (!in.u32(length)) {
    return false;
  }
  if (length == 0 || length > limits.max_frame_payload_bytes) {
    return false;
  }
  std::span<const std::byte> raw;
  if (!in.bytes(length, raw)) {
    return false;
  }
  Outcome<TopologySnapshot> decoded = decode_snapshot(raw, limits);
  if (!decoded.has_value()) {
    return false;
  }
  out = std::move(decoded).value();
  return true;
}

Outcome<TopologySnapshot> decode_snapshot(std::span<const std::byte> data, const Limits& limits) {
  Decoder in(data);
  TopologySnapshot staged;
  if (!in.u32(staged.format_version)) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot header is truncated");
  }
  if (staged.format_version != kSnapshotFormatVersion) {
    return make_error<TopologySnapshot>(ErrorCode::UnsupportedVersion,
                                        "snapshot format version " + std::to_string(staged.format_version) +
                                            " is not supported");
  }
  if (!in.id128(staged.registry)) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot registry identity is truncated");
  }
  std::span<const std::byte> digest_bytes;
  if (!in.bytes(digest_bytes, static_cast<std::uint32_t>(staged.provenance_digest.bytes.size()))) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot provenance digest is truncated");
  }
  if (digest_bytes.size() != staged.provenance_digest.bytes.size()) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot provenance digest has the wrong size");
  }
  std::memcpy(staged.provenance_digest.bytes.data(), digest_bytes.data(), staged.provenance_digest.bytes.size());
  if (!decode_summary(in, staged.summary)) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot summary is truncated");
  }

  std::uint32_t object_count = 0;
  if (!in.u32(object_count) || object_count > limits.max_snapshot_entries) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot object count is out of bounds");
  }
  staged.objects.reserve(object_count);
  for (std::uint32_t index = 0; index < object_count; ++index) {
    SnapshotObject element;
    if (!decode_object(in, limits, element)) {
      return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot object is malformed");
    }
    staged.objects.push_back(std::move(element));
  }

  std::uint32_t endpoint_count = 0;
  if (!in.u32(endpoint_count) || endpoint_count > limits.max_snapshot_entries) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot endpoint count is out of bounds");
  }
  staged.endpoints.reserve(endpoint_count);
  for (std::uint32_t index = 0; index < endpoint_count; ++index) {
    SnapshotEndpoint element;
    if (!decode_endpoint(in, limits, element)) {
      return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot endpoint is malformed");
    }
    staged.endpoints.push_back(std::move(element));
  }

  std::uint32_t port_count = 0;
  if (!in.u32(port_count) || port_count > limits.max_snapshot_entries) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot port count is out of bounds");
  }
  staged.ports.reserve(port_count);
  for (std::uint32_t index = 0; index < port_count; ++index) {
    SnapshotPort element;
    if (!decode_port(in, limits, element)) {
      return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot port is malformed");
    }
    staged.ports.push_back(std::move(element));
  }

  std::uint32_t fenced_session_count = 0;
  if (!in.u32(fenced_session_count) || fenced_session_count > limits.max_snapshot_entries) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot fenced session count is out of bounds");
  }
  staged.fenced_sessions.reserve(fenced_session_count);
  for (std::uint32_t index = 0; index < fenced_session_count; ++index) {
    SnapshotFencedSession element;
    if (!in.id128(element.source) || !in.u64(element.incarnation.value) || !in.u64(element.fenced_by.value) ||
        !in.u64(element.claims)) {
      return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot fenced session is malformed");
    }
    staged.fenced_sessions.push_back(element);
  }

  std::uint32_t fenced_object_count = 0;
  if (!in.u32(fenced_object_count) || fenced_object_count > limits.max_snapshot_entries) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot fenced object count is out of bounds");
  }
  staged.fenced_objects.reserve(fenced_object_count);
  for (std::uint32_t index = 0; index < fenced_object_count; ++index) {
    SnapshotFencedObject element;
    if (!in.id128(element.object) || !in.u64(element.current_incarnation.value) || !in.u64(element.fenced_claims)) {
      return make_error<TopologySnapshot>(ErrorCode::CorruptStore, "snapshot fenced object is malformed");
    }
    staged.fenced_objects.push_back(element);
  }

  if (!in.done()) {
    return make_error<TopologySnapshot>(ErrorCode::CorruptStore,
                                        "snapshot carries " + std::to_string(in.remaining()) + " trailing bytes");
  }
  return staged;
}

} // namespace cable_registry
