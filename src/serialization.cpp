// Cable Attachment Registry — canonical binary encoding of domain values.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every decode path validates before it allocates: a declared collection
// length is checked against the caller's Limits, and every string is read
// through a bounded accessor. A malformed buffer fails the decode and leaves
// the destination untouched.

#include "cable_registry/serialization.hpp"

#include <cstdint>
#include <utility>

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

void encode_authority(Encoder& out, const Authority& value) {
  out.u8(static_cast<std::uint8_t>(value.authority_class));
  out.u32(value.rank);
}

bool decode_authority(Decoder& in, Authority& out) {
  std::uint8_t authority_class = 0;
  std::uint32_t rank = 0;
  if (!in.u8(authority_class) || !in.u32(rank)) {
    return false;
  }
  if (authority_class > static_cast<std::uint8_t>(AuthorityClass::Authoritative)) {
    return false;
  }
  out.authority_class = static_cast<AuthorityClass>(authority_class);
  out.rank = rank;
  return true;
}

void encode_side_descriptor(Encoder& out, const ObjectSideDescriptor& value) {
  out.u8(static_cast<std::uint8_t>(value.connector));
  out.u8(static_cast<std::uint8_t>(value.media));
  out.u16(value.lanes.value);
}

bool decode_side_descriptor(Decoder& in, ObjectSideDescriptor& out) {
  ObjectSideDescriptor staged;
  std::uint8_t connector = 0;
  std::uint8_t media = 0;
  if (!in.u8(connector) || !in.u8(media) || !in.u16(staged.lanes.value)) {
    return false;
  }
  if (connector > static_cast<std::uint8_t>(ConnectorClass::Other) ||
      media > static_cast<std::uint8_t>(MediaClass::Other)) {
    return false;
  }
  staged.connector = static_cast<ConnectorClass>(connector);
  staged.media = static_cast<MediaClass>(media);
  out = staged;
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

void encode_recovery_report(Encoder& out, const RecoveryReport& value, const Limits& limits) {
  out.boolean(value.opened);
  out.boolean(value.created);
  out.boolean(value.salvaged);
  out.boolean(value.chain_intact);
  out.u32(value.format_version);
  out.u64(value.records_loaded);
  out.u64(value.records_rejected);
  out.u64(value.bytes_loaded);
  out.u64(value.bytes_discarded);
  out.u8(static_cast<std::uint8_t>(value.issue));
  out.u64(value.issue_offset);
  out.string(value.detail, limits.max_string_bytes);
}

bool decode_recovery_report(Decoder& in, const Limits& limits, RecoveryReport& out) {
  RecoveryReport staged;
  std::uint8_t issue = 0;
  if (!in.boolean(staged.opened) || !in.boolean(staged.created) || !in.boolean(staged.salvaged) ||
      !in.boolean(staged.chain_intact) || !in.u32(staged.format_version) || !in.u64(staged.records_loaded) ||
      !in.u64(staged.records_rejected) || !in.u64(staged.bytes_loaded) || !in.u64(staged.bytes_discarded) ||
      !in.u8(issue) || !in.u64(staged.issue_offset) || !in.string(staged.detail, limits.max_string_bytes)) {
    return false;
  }
  if (issue > static_cast<std::uint8_t>(RecoveryIssue::IoFailure)) {
    return false;
  }
  staged.issue = static_cast<RecoveryIssue>(issue);
  out = std::move(staged);
  return true;
}

bool decode_object_side_ref(Decoder& in, ObjectSideRef& out) {
  ObjectSideRef staged;
  if (!in.id128(staged.object) || !in.u32(staged.side.value)) {
    return false;
  }
  out = staged;
  return true;
}

void encode_object_side_ref(Encoder& out, const ObjectSideRef& value) {
  out.id128(value.object.value());
  out.u32(value.side.value);
}

bool decode_port_ref(Decoder& in, PortRef& out) {
  PortRef staged;
  if (!in.id128(staged.endpoint) || !in.u32(staged.index.value)) {
    return false;
  }
  out = staged;
  return true;
}

void encode_port_ref(Encoder& out, const PortRef& value) {
  out.id128(value.endpoint.value());
  out.u32(value.index.value);
}

} // namespace

void encode_source_descriptor(Encoder& out, const SourceDescriptor& value, const Limits& limits) {
  out.id128(value.id.value());
  out.string(value.name, limits.max_string_bytes);
  encode_authority(out, value.authority);
  out.string(value.administrative_domain, limits.max_string_bytes);
}

bool decode_source_descriptor(Decoder& in, const Limits& limits, SourceDescriptor& out) {
  SourceDescriptor staged;
  if (!in.id128(staged.id) || !in.string(staged.name, limits.max_string_bytes) ||
      !decode_authority(in, staged.authority) ||
      !in.string(staged.administrative_domain, limits.max_string_bytes)) {
    return false;
  }
  if (staged.id.is_nil()) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_capability(Encoder& out, const NominalCapability& value, const Limits& limits) {
  out.u8(static_cast<std::uint8_t>(value.media));
  out.u8(static_cast<std::uint8_t>(value.connector));
  out.u16(value.lanes.value);
  out.u64(value.nominal_lane_rate_kbps);
  out.u64(value.nominal_total_kbps);
  out.u64(value.nominal_reach_mm);
  out.boolean(value.reach_declared);
  out.string(value.capability_code, limits.max_capability_code_bytes);
}

bool decode_capability(Decoder& in, const Limits& limits, NominalCapability& out) {
  NominalCapability staged;
  std::uint8_t media = 0;
  std::uint8_t connector = 0;
  if (!in.u8(media) || !in.u8(connector) || !in.u16(staged.lanes.value) || !in.u64(staged.nominal_lane_rate_kbps) ||
      !in.u64(staged.nominal_total_kbps) || !in.u64(staged.nominal_reach_mm) || !in.boolean(staged.reach_declared) ||
      !in.string(staged.capability_code, limits.max_capability_code_bytes)) {
    return false;
  }
  if (media > static_cast<std::uint8_t>(MediaClass::Other) ||
      connector > static_cast<std::uint8_t>(ConnectorClass::Other)) {
    return false;
  }
  staged.media = static_cast<MediaClass>(media);
  staged.connector = static_cast<ConnectorClass>(connector);
  out = std::move(staged);
  return true;
}

void encode_port_capability(Encoder& out, const PortCapability& value, const Limits& limits) {
  (void)limits;
  out.u32(value.max_simultaneous_attachments);
  out.u8(static_cast<std::uint8_t>(value.accepted_connector));
  out.u8(static_cast<std::uint8_t>(value.accepted_media));
  out.u16(value.lane_capacity.value);
}

bool decode_port_capability(Decoder& in, const Limits& limits, PortCapability& out) {
  PortCapability staged;
  std::uint8_t connector = 0;
  std::uint8_t media = 0;
  if (!in.u32(staged.max_simultaneous_attachments) || !in.u8(connector) || !in.u8(media) ||
      !in.u16(staged.lane_capacity.value)) {
    return false;
  }
  if (staged.max_simultaneous_attachments == 0 ||
      staged.max_simultaneous_attachments > limits.max_port_simultaneous_attachments) {
    return false;
  }
  if (connector > static_cast<std::uint8_t>(ConnectorClass::Other) ||
      media > static_cast<std::uint8_t>(MediaClass::Other)) {
    return false;
  }
  staged.accepted_connector = static_cast<ConnectorClass>(connector);
  staged.accepted_media = static_cast<MediaClass>(media);
  out = staged;
  return true;
}

void encode_object_descriptor(Encoder& out, const ObjectDescriptor& value, const Limits& limits) {
  out.id128(value.id.value());
  out.u8(static_cast<std::uint8_t>(value.kind));
  out.string(value.physical_label, limits.max_string_bytes);
  out.string(value.serial_like, limits.max_string_bytes);
  out.string(value.administrative_location, limits.max_string_bytes);
  out.u32(static_cast<std::uint32_t>(value.sides.size()));
  for (const ObjectSideDescriptor& side : value.sides) {
    encode_side_descriptor(out, side);
  }
  encode_capability(out, value.capability, limits);
}

bool decode_object_descriptor(Decoder& in, const Limits& limits, ObjectDescriptor& out) {
  ObjectDescriptor staged;
  std::uint8_t kind = 0;
  if (!in.id128(staged.id) || !in.u8(kind) || !in.string(staged.physical_label, limits.max_string_bytes) ||
      !in.string(staged.serial_like, limits.max_string_bytes) ||
      !in.string(staged.administrative_location, limits.max_string_bytes)) {
    return false;
  }
  if (kind > static_cast<std::uint8_t>(ObjectKind::Other) || staged.id.is_nil()) {
    return false;
  }
  staged.kind = static_cast<ObjectKind>(kind);
  if (!decode_vector<ObjectSideDescriptor>(in, limits.max_sides_per_object, staged.sides,
                                           [&](ObjectSideDescriptor& element) {
                                             return decode_side_descriptor(in, element);
                                           })) {
    return false;
  }
  if (staged.sides.empty()) {
    return false;
  }
  if (!decode_capability(in, limits, staged.capability)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_endpoint_descriptor(Encoder& out, const EndpointDescriptor& value, const Limits& limits) {
  out.id128(value.id.value());
  out.u8(static_cast<std::uint8_t>(value.kind));
  out.string(value.name, limits.max_string_bytes);
  out.string(value.administrative_location, limits.max_string_bytes);
  out.u32(value.port_count);
  encode_port_capability(out, value.default_port_capability, limits);
}

bool decode_endpoint_descriptor(Decoder& in, const Limits& limits, EndpointDescriptor& out) {
  EndpointDescriptor staged;
  std::uint8_t kind = 0;
  if (!in.id128(staged.id) || !in.u8(kind) || !in.string(staged.name, limits.max_string_bytes) ||
      !in.string(staged.administrative_location, limits.max_string_bytes) || !in.u32(staged.port_count)) {
    return false;
  }
  if (kind > static_cast<std::uint8_t>(EndpointKind::Other) || staged.id.is_nil() || staged.port_count == 0 ||
      staged.port_count > limits.max_ports_per_endpoint) {
    return false;
  }
  staged.kind = static_cast<EndpointKind>(kind);
  if (!decode_port_capability(in, limits, staged.default_port_capability)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_evidence(Encoder& out, const Evidence& value, const Limits& limits) {
  out.id128(value.header.id.value());
  out.id128(value.header.source.value());
  out.u64(value.header.incarnation.value);
  out.u64(value.header.generation.value);
  out.i64(value.header.observed_at.unix_nanos);
  out.u8(static_cast<std::uint8_t>(value.header.provenance));
  out.string(value.header.provenance_detail, limits.max_string_bytes);
  out.u32(value.header.schema_version);

  const EvidenceKind kind = kind_of(value.payload);
  out.u16(static_cast<std::uint16_t>(kind));
  switch (kind) {
    case EvidenceKind::RegisterObject:
      encode_object_descriptor(out, std::get<RegisterObjectPayload>(value.payload).descriptor, limits);
      break;
    case EvidenceKind::RegisterEndpoint:
      encode_endpoint_descriptor(out, std::get<RegisterEndpointPayload>(value.payload).descriptor, limits);
      break;
    case EvidenceKind::Attach: {
      const auto& payload = std::get<AttachPayload>(value.payload);
      encode_object_side_ref(out, payload.subject);
      encode_port_ref(out, payload.port);
      out.u64(payload.object_incarnation.value);
      out.u32(payload.slot.value);
      break;
    }
    case EvidenceKind::Detach: {
      const auto& payload = std::get<DetachPayload>(value.payload);
      encode_object_side_ref(out, payload.subject);
      out.u64(payload.object_incarnation.value);
      out.boolean(payload.has_from_port);
      encode_port_ref(out, payload.from_port);
      out.u32(payload.slot.value);
      break;
    }
    case EvidenceKind::Move: {
      const auto& payload = std::get<MovePayload>(value.payload);
      encode_object_side_ref(out, payload.subject);
      out.u64(payload.object_incarnation.value);
      encode_port_ref(out, payload.from);
      encode_port_ref(out, payload.to);
      out.u32(payload.from_slot.value);
      out.u32(payload.to_slot.value);
      break;
    }
    case EvidenceKind::PublishObjectCapability: {
      const auto& payload = std::get<PublishObjectCapabilityPayload>(value.payload);
      out.id128(payload.object.value());
      encode_capability(out, payload.capability, limits);
      break;
    }
    case EvidenceKind::PublishPortCapability: {
      const auto& payload = std::get<PublishPortCapabilityPayload>(value.payload);
      encode_port_ref(out, payload.port);
      encode_port_capability(out, payload.capability, limits);
      break;
    }
    case EvidenceKind::SetLifecycle: {
      const auto& payload = std::get<SetLifecyclePayload>(value.payload);
      out.id128(payload.object.value());
      out.u64(payload.object_incarnation.value);
      out.u8(static_cast<std::uint8_t>(payload.action));
      out.string(payload.reason, limits.max_string_bytes);
      break;
    }
    case EvidenceKind::ReincarnateObject: {
      const auto& payload = std::get<ReincarnateObjectPayload>(value.payload);
      out.id128(payload.object.value());
      out.u64(payload.from_incarnation.value);
      out.string(payload.new_serial_like, limits.max_string_bytes);
      out.string(payload.reason, limits.max_string_bytes);
      break;
    }
  }
}

bool decode_evidence(Decoder& in, const Limits& limits, Evidence& out) {
  Evidence staged;
  if (!in.id128(staged.header.id) || !in.id128(staged.header.source) || !in.u64(staged.header.incarnation.value) ||
      !in.u64(staged.header.generation.value) || !in.i64(staged.header.observed_at.unix_nanos)) {
    return false;
  }
  std::uint8_t provenance = 0;
  if (!in.u8(provenance) || !in.string(staged.header.provenance_detail, limits.max_string_bytes) ||
      !in.u32(staged.header.schema_version)) {
    return false;
  }
  if (provenance > static_cast<std::uint8_t>(ProvenanceClass::Synthetic)) {
    return false;
  }
  staged.header.provenance = static_cast<ProvenanceClass>(provenance);

  if (staged.header.id.is_nil() || staged.header.source.is_nil()) {
    return false;
  }
  if (staged.header.incarnation.value == 0 || staged.header.generation.value == 0) {
    return false;
  }
  if (staged.header.schema_version != kEvidenceSchemaVersion) {
    return false;
  }

  std::uint16_t kind_raw = 0;
  if (!in.u16(kind_raw)) {
    return false;
  }
  const auto kind = static_cast<EvidenceKind>(kind_raw);
  switch (kind) {
    case EvidenceKind::RegisterObject: {
      RegisterObjectPayload payload;
      if (!decode_object_descriptor(in, limits, payload.descriptor)) {
        return false;
      }
      staged.payload = std::move(payload);
      break;
    }
    case EvidenceKind::RegisterEndpoint: {
      RegisterEndpointPayload payload;
      if (!decode_endpoint_descriptor(in, limits, payload.descriptor)) {
        return false;
      }
      staged.payload = std::move(payload);
      break;
    }
    case EvidenceKind::Attach: {
      AttachPayload payload;
      if (!decode_object_side_ref(in, payload.subject) || !decode_port_ref(in, payload.port) ||
          !in.u64(payload.object_incarnation.value) || !in.u32(payload.slot.value)) {
        return false;
      }
      if (payload.object_incarnation.value == 0) {
        return false;
      }
      staged.payload = std::move(payload);
      break;
    }
    case EvidenceKind::Detach: {
      DetachPayload payload;
      if (!decode_object_side_ref(in, payload.subject) || !in.u64(payload.object_incarnation.value) ||
          !in.boolean(payload.has_from_port) || !decode_port_ref(in, payload.from_port) ||
          !in.u32(payload.slot.value)) {
        return false;
      }
      if (payload.object_incarnation.value == 0) {
        return false;
      }
      staged.payload = std::move(payload);
      break;
    }
    case EvidenceKind::Move: {
      MovePayload payload;
      if (!decode_object_side_ref(in, payload.subject) || !in.u64(payload.object_incarnation.value) ||
          !decode_port_ref(in, payload.from) || !decode_port_ref(in, payload.to) ||
          !in.u32(payload.from_slot.value) || !in.u32(payload.to_slot.value)) {
        return false;
      }
      if (payload.object_incarnation.value == 0) {
        return false;
      }
      staged.payload = std::move(payload);
      break;
    }
    case EvidenceKind::PublishObjectCapability: {
      PublishObjectCapabilityPayload payload;
      if (!in.id128(payload.object) || !decode_capability(in, limits, payload.capability)) {
        return false;
      }
      staged.payload = std::move(payload);
      break;
    }
    case EvidenceKind::PublishPortCapability: {
      PublishPortCapabilityPayload payload;
      if (!decode_port_ref(in, payload.port) || !decode_port_capability(in, limits, payload.capability)) {
        return false;
      }
      staged.payload = std::move(payload);
      break;
    }
    case EvidenceKind::SetLifecycle: {
      SetLifecyclePayload payload;
      std::uint8_t action = 0;
      if (!in.id128(payload.object) || !in.u64(payload.object_incarnation.value) || !in.u8(action) ||
          !in.string(payload.reason, limits.max_string_bytes)) {
        return false;
      }
      if (action > static_cast<std::uint8_t>(LifecycleAction::Retired) || payload.object_incarnation.value == 0) {
        return false;
      }
      payload.action = static_cast<LifecycleAction>(action);
      staged.payload = std::move(payload);
      break;
    }
    case EvidenceKind::ReincarnateObject: {
      ReincarnateObjectPayload payload;
      if (!in.id128(payload.object) || !in.u64(payload.from_incarnation.value) ||
          !in.string(payload.new_serial_like, limits.max_string_bytes) ||
          !in.string(payload.reason, limits.max_string_bytes)) {
        return false;
      }
      if (payload.from_incarnation.value == 0) {
        return false;
      }
      staged.payload = std::move(payload);
      break;
    }
    default:
      return false;
  }
  out = std::move(staged);
  return true;
}

std::vector<std::byte> encode_evidence(const Evidence& value, const Limits& limits) {
  Encoder encoder;
  encode_evidence(encoder, value, limits);
  return encoder.take();
}

Outcome<Evidence> decode_evidence(std::span<const std::byte> data, const Limits& limits) {
  Decoder decoder(data);
  Evidence evidence;
  if (!decode_evidence(decoder, limits, evidence)) {
    return make_error<Evidence>(ErrorCode::InvalidArgument, "evidence record is malformed or truncated");
  }
  if (!decoder.done()) {
    return make_error<Evidence>(ErrorCode::InvalidArgument,
                                "evidence record carries " + std::to_string(decoder.remaining()) +
                                    " trailing bytes");
  }
  return evidence;
}

void encode_claim_provenance(Encoder& out, const ClaimProvenance& value) {
  out.id128(value.source.value());
  out.u64(value.incarnation.value);
  out.u64(value.generation.value);
  out.id128(value.evidence.value());
  out.i64(value.observed_at.unix_nanos);
  out.i64(value.received_at.unix_nanos);
  out.u8(static_cast<std::uint8_t>(value.provenance));
  encode_authority(out, value.authority);
  out.u64(value.object_incarnation.value);
  out.boolean(value.asserted_in_lifetime);
}

bool decode_claim_provenance(Decoder& in, ClaimProvenance& out) {
  ClaimProvenance staged;
  std::uint8_t provenance = 0;
  if (!in.id128(staged.source) || !in.u64(staged.incarnation.value) || !in.u64(staged.generation.value) ||
      !in.id128(staged.evidence) || !in.i64(staged.observed_at.unix_nanos) ||
      !in.i64(staged.received_at.unix_nanos) || !in.u8(provenance) || !decode_authority(in, staged.authority) ||
      !in.u64(staged.object_incarnation.value) || !in.boolean(staged.asserted_in_lifetime)) {
    return false;
  }
  if (provenance > static_cast<std::uint8_t>(ProvenanceClass::Synthetic)) {
    return false;
  }
  staged.provenance = static_cast<ProvenanceClass>(provenance);
  out = staged;
  return true;
}

void encode_conflict_note(Encoder& out, const ConflictNote& value, const Limits& limits) {
  out.u8(static_cast<std::uint8_t>(value.reason));
  encode_object_side_ref(out, value.subject);
  encode_port_ref(out, value.port);
  out.string(value.detail, limits.max_string_bytes);
  out.u32(static_cast<std::uint32_t>(value.claims.size()));
  for (const ClaimProvenance& claim : value.claims) {
    encode_claim_provenance(out, claim);
  }
}

bool decode_conflict_note(Decoder& in, const Limits& limits, ConflictNote& out) {
  ConflictNote staged;
  std::uint8_t reason = 0;
  if (!in.u8(reason) || !decode_object_side_ref(in, staged.subject) || !decode_port_ref(in, staged.port) ||
      !in.string(staged.detail, limits.max_string_bytes)) {
    return false;
  }
  if (reason > static_cast<std::uint8_t>(ConflictReason::IncompatibleMedia)) {
    return false;
  }
  staged.reason = static_cast<ConflictReason>(reason);
  if (!decode_vector<ClaimProvenance>(in, limits.max_claim_records_per_port, staged.claims,
                                      [&](ClaimProvenance& element) { return decode_claim_provenance(in, element); })) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_attachment_edge(Encoder& out, const AttachmentEdge& value, const Limits& limits) {
  encode_object_side_ref(out, value.subject);
  encode_port_ref(out, value.port);
  out.u32(value.slot.value);
  encode_authority(out, value.authority);
  out.u8(static_cast<std::uint8_t>(value.validation));
  out.boolean(value.connector_compatible);
  out.boolean(value.media_compatible);
  out.boolean(value.conflicting);
  out.u64(value.object_incarnation.value);
  out.u32(static_cast<std::uint32_t>(value.claims.size()));
  for (const ClaimProvenance& claim : value.claims) {
    encode_claim_provenance(out, claim);
  }
  out.u32(static_cast<std::uint32_t>(value.overridden.size()));
  for (const ClaimProvenance& claim : value.overridden) {
    encode_claim_provenance(out, claim);
  }
  out.u32(value.overridden_total);
  (void)limits;
}

bool decode_attachment_edge(Decoder& in, const Limits& limits, AttachmentEdge& out) {
  AttachmentEdge staged;
  std::uint8_t validation = 0;
  if (!decode_object_side_ref(in, staged.subject) || !decode_port_ref(in, staged.port) ||
      !in.u32(staged.slot.value) || !decode_authority(in, staged.authority) || !in.u8(validation) ||
      !in.boolean(staged.connector_compatible) || !in.boolean(staged.media_compatible) ||
      !in.boolean(staged.conflicting) || !in.u64(staged.object_incarnation.value)) {
    return false;
  }
  if (validation > static_cast<std::uint8_t>(ValidationState::Validated)) {
    return false;
  }
  staged.validation = static_cast<ValidationState>(validation);
  if (!decode_vector<ClaimProvenance>(in, limits.max_claim_records_per_port, staged.claims,
                                      [&](ClaimProvenance& element) { return decode_claim_provenance(in, element); })) {
    return false;
  }
  if (!decode_vector<ClaimProvenance>(
          in, limits.max_history_per_key * limits.max_claim_records_per_port, staged.overridden,
          [&](ClaimProvenance& element) { return decode_claim_provenance(in, element); })) {
    return false;
  }
  if (!in.u32(staged.overridden_total)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_port_view(Encoder& out, const PortView& value, const Limits& limits) {
  encode_port_ref(out, value.port);
  out.u8(static_cast<std::uint8_t>(value.state));
  out.u8(static_cast<std::uint8_t>(value.validation));
  encode_port_capability(out, value.capability, limits);
  out.u32(static_cast<std::uint32_t>(value.edges.size()));
  for (const AttachmentEdge& edge : value.edges) {
    encode_attachment_edge(out, edge, limits);
  }
  out.u32(static_cast<std::uint32_t>(value.conflicts.size()));
  for (const ConflictNote& note : value.conflicts) {
    encode_conflict_note(out, note, limits);
  }
  out.u32(static_cast<std::uint32_t>(value.emptiness_claims.size()));
  for (const ClaimProvenance& claim : value.emptiness_claims) {
    encode_claim_provenance(out, claim);
  }
  out.u32(value.emptiness_claims_total);
}

bool decode_port_view(Decoder& in, const Limits& limits, PortView& out) {
  PortView staged;
  std::uint8_t state = 0;
  std::uint8_t validation = 0;
  if (!decode_port_ref(in, staged.port) || !in.u8(state) || !in.u8(validation) ||
      !decode_port_capability(in, limits, staged.capability)) {
    return false;
  }
  if (state > static_cast<std::uint8_t>(PortAttachmentState::Conflicting) ||
      validation > static_cast<std::uint8_t>(ValidationState::Validated)) {
    return false;
  }
  staged.state = static_cast<PortAttachmentState>(state);
  staged.validation = static_cast<ValidationState>(validation);
  if (!decode_vector<AttachmentEdge>(
          in, limits.max_claim_records_per_port, staged.edges,
          [&](AttachmentEdge& element) { return decode_attachment_edge(in, limits, element); })) {
    return false;
  }
  if (!decode_vector<ConflictNote>(in, limits.max_claim_records_per_port, staged.conflicts,
                                   [&](ConflictNote& element) { return decode_conflict_note(in, limits, element); })) {
    return false;
  }
  if (!decode_vector<ClaimProvenance>(
          in, limits.max_claim_records_per_port, staged.emptiness_claims,
          [&](ClaimProvenance& element) { return decode_claim_provenance(in, element); })) {
    return false;
  }
  if (!in.u32(staged.emptiness_claims_total)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_lifecycle_event(Encoder& out, const LifecycleEvent& value, const Limits& limits) {
  out.u8(static_cast<std::uint8_t>(value.kind));
  out.i64(value.observed_at.unix_nanos);
  out.i64(value.received_at.unix_nanos);
  out.id128(value.source.value());
  out.u64(value.incarnation.value);
  out.u64(value.generation.value);
  out.id128(value.evidence.value());
  out.u8(static_cast<std::uint8_t>(value.provenance));
  out.u64(value.object_incarnation.value);
  out.u32(value.side.value);
  encode_port_ref(out, value.port);
  encode_port_ref(out, value.from_port);
  out.boolean(value.has_from_port);
  out.string(value.detail, limits.max_string_bytes);
}

bool decode_lifecycle_event(Decoder& in, const Limits& limits, LifecycleEvent& out) {
  LifecycleEvent staged;
  std::uint8_t kind = 0;
  std::uint8_t provenance = 0;
  if (!in.u8(kind) || !in.i64(staged.observed_at.unix_nanos) || !in.i64(staged.received_at.unix_nanos) ||
      !in.id128(staged.source) || !in.u64(staged.incarnation.value) || !in.u64(staged.generation.value) ||
      !in.id128(staged.evidence) || !in.u8(provenance) || !in.u64(staged.object_incarnation.value) ||
      !in.u32(staged.side.value) || !decode_port_ref(in, staged.port) || !decode_port_ref(in, staged.from_port) ||
      !in.boolean(staged.has_from_port) || !in.string(staged.detail, limits.max_string_bytes)) {
    return false;
  }
  if (kind > static_cast<std::uint8_t>(LifecycleEventKind::Retired) ||
      provenance > static_cast<std::uint8_t>(ProvenanceClass::Synthetic)) {
    return false;
  }
  staged.kind = static_cast<LifecycleEventKind>(kind);
  staged.provenance = static_cast<ProvenanceClass>(provenance);
  out = std::move(staged);
  return true;
}

void encode_object_view(Encoder& out, const ObjectView& value, const Limits& limits) {
  out.id128(value.id.value());
  out.u8(static_cast<std::uint8_t>(value.kind));
  out.string(value.physical_label, limits.max_string_bytes);
  out.string(value.serial_like, limits.max_string_bytes);
  out.string(value.administrative_location, limits.max_string_bytes);
  out.u64(value.incarnation.value);
  out.u8(static_cast<std::uint8_t>(value.lifecycle));
  encode_capability(out, value.capability, limits);
  out.u32(static_cast<std::uint32_t>(value.sides.size()));
  for (const ObjectSideView& side : value.sides) {
    out.u32(side.side.value);
    encode_side_descriptor(out, side.descriptor);
    out.u8(static_cast<std::uint8_t>(side.state));
    out.u8(static_cast<std::uint8_t>(side.validation));
    out.u32(static_cast<std::uint32_t>(side.edges.size()));
    for (const AttachmentEdge& edge : side.edges) {
      encode_attachment_edge(out, edge, limits);
    }
    out.u32(static_cast<std::uint32_t>(side.conflicts.size()));
    for (const ConflictNote& note : side.conflicts) {
      encode_conflict_note(out, note, limits);
    }
  }
  out.u32(static_cast<std::uint32_t>(value.history.size()));
  for (const LifecycleEvent& event : value.history) {
    encode_lifecycle_event(out, event, limits);
  }
  out.u32(value.history_dropped);
  out.u32(static_cast<std::uint32_t>(value.conflicts.size()));
  for (const ConflictNote& note : value.conflicts) {
    encode_conflict_note(out, note, limits);
  }
  out.u8(static_cast<std::uint8_t>(value.validation));
}

bool decode_object_view(Decoder& in, const Limits& limits, ObjectView& out) {
  ObjectView staged;
  std::uint8_t kind = 0;
  std::uint8_t lifecycle = 0;
  std::uint8_t validation = 0;
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
  if (!decode_vector<ObjectSideView>(in, limits.max_sides_per_object, staged.sides, [&](ObjectSideView& side) {
        std::uint8_t side_state = 0;
        std::uint8_t side_validation = 0;
        if (!in.u32(side.side.value) || !decode_side_descriptor(in, side.descriptor) || !in.u8(side_state) ||
            !in.u8(side_validation)) {
          return false;
        }
        if (side_state > static_cast<std::uint8_t>(PortAttachmentState::Conflicting) ||
            side_validation > static_cast<std::uint8_t>(ValidationState::Validated)) {
          return false;
        }
        side.state = static_cast<PortAttachmentState>(side_state);
        side.validation = static_cast<ValidationState>(side_validation);
        if (!decode_vector<AttachmentEdge>(
                in, limits.max_claim_records_per_port, side.edges,
                [&](AttachmentEdge& element) { return decode_attachment_edge(in, limits, element); })) {
          return false;
        }
        return decode_vector<ConflictNote>(
            in, limits.max_claim_records_per_port, side.conflicts,
            [&](ConflictNote& element) { return decode_conflict_note(in, limits, element); });
      })) {
    return false;
  }
  if (!decode_vector<LifecycleEvent>(in, limits.max_history_events, staged.history, [&](LifecycleEvent& element) {
        return decode_lifecycle_event(in, limits, element);
      })) {
    return false;
  }
  if (!in.u32(staged.history_dropped)) {
    return false;
  }
  if (!decode_vector<ConflictNote>(in, limits.max_claim_records_per_port, staged.conflicts,
                                   [&](ConflictNote& element) { return decode_conflict_note(in, limits, element); })) {
    return false;
  }
  if (!in.u8(validation) || validation > static_cast<std::uint8_t>(ValidationState::Validated)) {
    return false;
  }
  staged.validation = static_cast<ValidationState>(validation);
  out = std::move(staged);
  return true;
}

void encode_endpoint_view(Encoder& out, const EndpointView& value, const Limits& limits) {
  out.id128(value.id.value());
  out.u8(static_cast<std::uint8_t>(value.kind));
  out.string(value.name, limits.max_string_bytes);
  out.string(value.administrative_location, limits.max_string_bytes);
  out.u32(value.port_count);
  encode_port_capability(out, value.default_port_capability, limits);
  out.u32(static_cast<std::uint32_t>(value.ports.size()));
  for (const PortView& port : value.ports) {
    encode_port_view(out, port, limits);
  }
}

bool decode_endpoint_view(Decoder& in, const Limits& limits, EndpointView& out) {
  EndpointView staged;
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
  if (!decode_vector<PortView>(in, limits.max_ports_per_endpoint, staged.ports,
                               [&](PortView& element) { return decode_port_view(in, limits, element); })) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_object_history(Encoder& out, const ObjectHistory& value, const Limits& limits) {
  out.id128(value.object.value());
  out.u32(static_cast<std::uint32_t>(value.events.size()));
  for (const LifecycleEvent& event : value.events) {
    encode_lifecycle_event(out, event, limits);
  }
  out.u32(value.dropped_events);
}

bool decode_object_history(Decoder& in, const Limits& limits, ObjectHistory& out) {
  ObjectHistory staged;
  if (!in.id128(staged.object)) {
    return false;
  }
  if (!decode_vector<LifecycleEvent>(in, limits.max_history_events, staged.events,
                                     [&](LifecycleEvent& element) {
                                       return decode_lifecycle_event(in, limits, element);
                                     })) {
    return false;
  }
  if (!in.u32(staged.dropped_events)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_ingest_result(Encoder& out, const IngestResult& value, const Limits& limits) {
  out.u8(static_cast<std::uint8_t>(value.disposition));
  out.id128(value.evidence.value());
  out.u16(static_cast<std::uint16_t>(value.kind));
  out.u64(value.high_water.value);
  out.i64(value.received_at.unix_nanos);
  out.string(value.detail, limits.max_string_bytes);
}

bool decode_ingest_result(Decoder& in, const Limits& limits, IngestResult& out) {
  IngestResult staged;
  std::uint8_t disposition = 0;
  std::uint16_t kind = 0;
  if (!in.u8(disposition) || !in.id128(staged.evidence) || !in.u16(kind) ||
      !in.u64(staged.high_water.value) || !in.i64(staged.received_at.unix_nanos) ||
      !in.string(staged.detail, limits.max_string_bytes)) {
    return false;
  }
  if (disposition > static_cast<std::uint8_t>(IngestDisposition::RefusedPersistence)) {
    return false;
  }
  EvidenceKind parsed_kind = EvidenceKind::RegisterObject;
  switch (kind) {
    case 1:
      parsed_kind = EvidenceKind::RegisterObject;
      break;
    case 2:
      parsed_kind = EvidenceKind::RegisterEndpoint;
      break;
    case 3:
      parsed_kind = EvidenceKind::Attach;
      break;
    case 4:
      parsed_kind = EvidenceKind::Detach;
      break;
    case 5:
      parsed_kind = EvidenceKind::Move;
      break;
    case 6:
      parsed_kind = EvidenceKind::PublishObjectCapability;
      break;
    case 7:
      parsed_kind = EvidenceKind::PublishPortCapability;
      break;
    case 8:
      parsed_kind = EvidenceKind::SetLifecycle;
      break;
    case 9:
      parsed_kind = EvidenceKind::ReincarnateObject;
      break;
    default:
      return false;
  }
  staged.disposition = static_cast<IngestDisposition>(disposition);
  staged.kind = parsed_kind;
  out = std::move(staged);
  return true;
}

std::vector<std::byte> encode_ingest_results(std::span<const IngestResult> values, const Limits& limits) {
  Encoder encoder;
  encoder.u32(static_cast<std::uint32_t>(values.size()));
  for (const IngestResult& value : values) {
    encode_ingest_result(encoder, value, limits);
  }
  return encoder.take();
}

bool decode_ingest_results(Decoder& in, const Limits& limits, std::vector<IngestResult>& out) {
  return decode_vector<IngestResult>(in, limits.max_evidence_batch, out,
                                     [&](IngestResult& element) { return decode_ingest_result(in, limits, element); });
}

void encode_inspection_view(Encoder& out, const InspectionView& value, const Limits& limits) {
  out.u32(static_cast<std::uint32_t>(value.fenced_sessions.size()));
  for (const FencedSessionView& session : value.fenced_sessions) {
    out.id128(session.source.value());
    out.u64(session.incarnation.value);
    out.u64(session.fenced_by.value);
    out.u64(session.claims);
  }
  out.u32(static_cast<std::uint32_t>(value.fenced_objects.size()));
  for (const FencedObjectView& object : value.fenced_objects) {
    out.id128(object.object.value());
    out.u64(object.current_incarnation.value);
    out.u64(object.fenced_claims);
    out.string(object.physical_label, limits.max_string_bytes);
  }
  out.u32(static_cast<std::uint32_t>(value.unvalidated_ports.size()));
  for (const PortView& port : value.unvalidated_ports) {
    encode_port_view(out, port, limits);
  }
  out.u32(static_cast<std::uint32_t>(value.pending_references.size()));
  for (const PendingReferenceView& pending : value.pending_references) {
    out.id128(pending.evidence.value());
    out.u16(static_cast<std::uint16_t>(pending.kind));
    out.id128(pending.source.value());
    out.u64(pending.incarnation.value);
    out.u64(pending.generation.value);
    out.string(pending.detail, limits.max_string_bytes);
  }
  out.u32(static_cast<std::uint32_t>(value.refusals.size()));
  for (const RefusalView& refusal : value.refusals) {
    out.id128(refusal.evidence.value());
    out.u16(static_cast<std::uint16_t>(refusal.kind));
    out.id128(refusal.source.value());
    out.u64(refusal.incarnation.value);
    out.u64(refusal.generation.value);
    out.u8(static_cast<std::uint8_t>(refusal.disposition));
    out.string(refusal.detail, limits.max_string_bytes);
  }
  encode_summary(out, value.summary);
}

bool decode_inspection_view(Decoder& in, const Limits& limits, InspectionView& out) {
  InspectionView staged;
  if (!decode_vector<FencedSessionView>(in, limits.max_sources * 64u, staged.fenced_sessions,
                                        [&](FencedSessionView& element) {
                                          return in.id128(element.source) &&
                                                 in.u64(element.incarnation.value) &&
                                                 in.u64(element.fenced_by.value) && in.u64(element.claims);
                                        })) {
    return false;
  }
  if (!decode_vector<FencedObjectView>(in, limits.max_objects, staged.fenced_objects,
                                       [&](FencedObjectView& element) {
                                         return in.id128(element.object) &&
                                                in.u64(element.current_incarnation.value) &&
                                                in.u64(element.fenced_claims) &&
                                                in.string(element.physical_label, limits.max_string_bytes);
                                       })) {
    return false;
  }
  if (!decode_vector<PortView>(in, limits.max_snapshot_entries, staged.unvalidated_ports,
                               [&](PortView& element) { return decode_port_view(in, limits, element); })) {
    return false;
  }
  if (!decode_vector<PendingReferenceView>(in, limits.max_evidence_batch, staged.pending_references,
                                           [&](PendingReferenceView& element) {
                                             std::uint16_t kind = 0;
                                             if (!in.id128(element.evidence) || !in.u16(kind) ||
                                                 !in.id128(element.source) ||
                                                 !in.u64(element.incarnation.value) ||
                                                 !in.u64(element.generation.value) ||
                                                 !in.string(element.detail, limits.max_string_bytes)) {
                                               return false;
                                             }
                                             return kind >= 1 && kind <= 9;
                                           })) {
    return false;
  }
  if (!decode_vector<RefusalView>(in, limits.max_refusal_audit, staged.refusals,
                                  [&](RefusalView& element) {
                                    std::uint16_t kind = 0;
                                    std::uint8_t disposition = 0;
                                    if (!in.id128(element.evidence) || !in.u16(kind) ||
                                        !in.id128(element.source) || !in.u64(element.incarnation.value) ||
                                        !in.u64(element.generation.value) || !in.u8(disposition) ||
                                        !in.string(element.detail, limits.max_string_bytes)) {
                                      return false;
                                    }
                                    if (kind < 1 || kind > 9 ||
                                        disposition > static_cast<std::uint8_t>(
                                                          IngestDisposition::RefusedPersistence)) {
                                      return false;
                                    }
                                    return true;
                                  })) {
    return false;
  }
  if (!decode_summary(in, staged.summary)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_registry_stats(Encoder& out, const RegistryStats& value, const Limits& limits) {
  out.id128(value.registry.value());
  out.i64(value.opened_at.unix_nanos);
  out.u64(value.objects);
  out.u64(value.endpoints);
  out.u64(value.ports);
  out.u64(value.sources);
  out.u64(value.sessions);
  out.u64(value.live_sessions);
  out.u64(value.claim_records);
  out.u64(value.port_records);
  out.u64(value.lifecycle_records);
  out.u64(value.capability_records);
  out.u64(value.evidence_accepted);
  out.u64(value.evidence_refused);
  out.u64(value.evidence_duplicate);
  out.u64(value.evidence_superseded);
  out.u64(value.evidence_fenced);
  out.u64(value.evidence_pending);
  out.u64(value.store_records);
  out.u64(value.store_bytes);
  out.u64(value.store_rewrites);
  encode_recovery_report(out, value.recovery, limits);
}

bool decode_registry_stats(Decoder& in, const Limits& limits, RegistryStats& out) {
  RegistryStats staged;
  if (!in.id128(staged.registry) || !in.i64(staged.opened_at.unix_nanos) || !in.u64(staged.objects) ||
      !in.u64(staged.endpoints) || !in.u64(staged.ports) || !in.u64(staged.sources) || !in.u64(staged.sessions) ||
      !in.u64(staged.live_sessions) || !in.u64(staged.claim_records) || !in.u64(staged.port_records) ||
      !in.u64(staged.lifecycle_records) || !in.u64(staged.capability_records) || !in.u64(staged.evidence_accepted) ||
      !in.u64(staged.evidence_refused) || !in.u64(staged.evidence_duplicate) ||
      !in.u64(staged.evidence_superseded) || !in.u64(staged.evidence_fenced) || !in.u64(staged.evidence_pending) ||
      !in.u64(staged.store_records) || !in.u64(staged.store_bytes) || !in.u64(staged.store_rewrites)) {
    return false;
  }
  if (!decode_recovery_report(in, limits, staged.recovery)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_open_session_result(Encoder& out, const OpenSessionResult& value, const Limits& limits) {
  out.u8(static_cast<std::uint8_t>(value.disposition));
  out.id128(value.key.source.value());
  out.u64(value.key.incarnation.value);
  out.u64(value.previous_incarnation.value);
  out.u64(value.fenced_claims);
  out.string(value.detail, limits.max_string_bytes);
}

bool decode_open_session_result(Decoder& in, const Limits& limits, OpenSessionResult& out) {
  OpenSessionResult staged;
  std::uint8_t disposition = 0;
  if (!in.u8(disposition) || !in.id128(staged.key.source) || !in.u64(staged.key.incarnation.value) ||
      !in.u64(staged.previous_incarnation.value) || !in.u64(staged.fenced_claims) ||
      !in.string(staged.detail, limits.max_string_bytes)) {
    return false;
  }
  if (disposition > static_cast<std::uint8_t>(OpenSessionResult::Disposition::RefusedPersistence)) {
    return false;
  }
  staged.disposition = static_cast<OpenSessionResult::Disposition>(disposition);
  out = std::move(staged);
  return true;
}

void encode_query_options(Encoder& out, const QueryOptions& value) {
  out.u8(static_cast<std::uint8_t>(value.validation_policy));
  out.boolean(value.include_object_sides);
  out.boolean(value.include_history);
  out.boolean(value.include_endpoint_ports);
  out.id128(value.endpoint.value());
  out.id128(value.object.value());
}

bool decode_query_options(Decoder& in, QueryOptions& out) {
  QueryOptions staged;
  std::uint8_t policy = 0;
  if (!in.u8(policy) || !in.boolean(staged.include_object_sides) || !in.boolean(staged.include_history) ||
      !in.boolean(staged.include_endpoint_ports) || !in.id128(staged.endpoint) || !in.id128(staged.object)) {
    return false;
  }
  if (policy > static_cast<std::uint8_t>(ValidationPolicy::LiveOnly)) {
    return false;
  }
  staged.validation_policy = static_cast<ValidationPolicy>(policy);
  out = staged;
  return true;
}

} // namespace cable_registry
