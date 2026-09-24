// Cable Attachment Registry — internal state helpers and state encoding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "detail/registry_state.hpp"

#include <cstdint>
#include <utility>

#include "cable_registry/codec.hpp"
#include "cable_registry/limits.hpp"
#include "cable_registry/serialization.hpp"
#include "cable_registry/version.hpp"

namespace cable_registry::detail {
namespace {

void encode_owner(Encoder& out, const SourceIncarnationKey& owner) {
  out.id128(owner.source.value());
  out.u64(owner.incarnation.value);
}

bool decode_owner(Decoder& in, SourceIncarnationKey& out) {
  SourceIncarnationKey staged;
  if (!in.id128(staged.source) || !in.u64(staged.incarnation.value)) {
    return false;
  }
  if (staged.source.is_nil() || staged.incarnation.value == 0) {
    return false;
  }
  out = staged;
  return true;
}

void encode_attestation(Encoder& out, const Attestation& value, const Limits& limits) {
  encode_owner(out, value.owner);
  out.u64(value.generation.value);
  out.id128(value.evidence.value());
  out.i64(value.observed_at.unix_nanos);
  out.i64(value.received_at.unix_nanos);
  out.u8(static_cast<std::uint8_t>(value.provenance));
  out.u8(static_cast<std::uint8_t>(value.authority.authority_class));
  out.u32(value.authority.rank);
  out.u64(value.object_incarnation.value);
  out.boolean(value.asserted_in_lifetime);
  (void)limits;
}

bool decode_attestation(Decoder& in, Attestation& out) {
  Attestation staged;
  std::uint8_t provenance = 0;
  std::uint8_t authority_class = 0;
  if (!decode_owner(in, staged.owner) || !in.u64(staged.generation.value) || !in.id128(staged.evidence) ||
      !in.i64(staged.observed_at.unix_nanos) || !in.i64(staged.received_at.unix_nanos) || !in.u8(provenance) ||
      !in.u8(authority_class) || !in.u32(staged.authority.rank) ||
      !in.u64(staged.object_incarnation.value) || !in.boolean(staged.asserted_in_lifetime)) {
    return false;
  }
  if (provenance > static_cast<std::uint8_t>(ProvenanceClass::Synthetic) ||
      authority_class > static_cast<std::uint8_t>(AuthorityClass::Authoritative)) {
    return false;
  }
  staged.provenance = static_cast<ProvenanceClass>(provenance);
  staged.authority.authority_class = static_cast<AuthorityClass>(authority_class);
  out = staged;
  return true;
}

template <class T, class EncodeValue>
void encode_folded(Encoder& out, const Folded<T>& folded, EncodeValue encode_value, const Limits& limits) {
  encode_attestation(out, folded.meta, limits);
  encode_value(out, folded.value);
  out.u32(static_cast<std::uint32_t>(folded.superseded.size()));
  for (const Attestation& attestation : folded.superseded) {
    encode_attestation(out, attestation, limits);
  }
  out.u32(folded.history_dropped);
}

template <class T, class DecodeValue>
bool decode_folded(Decoder& in, const Limits& limits, std::uint32_t max_history, Folded<T>& out, DecodeValue decode_value) {
  (void)limits;
  Folded<T> staged;
  if (!decode_attestation(in, staged.meta)) {
    return false;
  }
  if (!decode_value(staged.value)) {
    return false;
  }
  std::uint32_t superseded_count = 0;
  if (!in.u32(superseded_count) || superseded_count > max_history) {
    return false;
  }
  staged.superseded.resize(superseded_count);
  for (std::uint32_t index = 0; index < superseded_count; ++index) {
    if (!decode_attestation(in, staged.superseded[index])) {
      return false;
    }
  }
  if (!in.u32(staged.history_dropped)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_object_id(Encoder& out, const ObjectId& value) {
  out.id128(value.value());
}

void encode_endpoint_id(Encoder& out, const EndpointId& value) {
  out.id128(value.value());
}

void encode_port_ref_public(Encoder& out, const PortRef& value) {
  encode_endpoint_id(out, value.endpoint);
  out.u32(value.index.value);
}

bool decode_port_ref(Decoder& in, PortRef& out) {
  PortRef staged;
  if (!in.id128(staged.endpoint) || !in.u32(staged.index.value)) {
    return false;
  }
  out = staged;
  return true;
}

void encode_object_side(Encoder& out, const ObjectSideRef& value) {
  encode_object_id(out, value.object);
  out.u32(value.side.value);
}

bool decode_object_side(Decoder& in, ObjectSideRef& out) {
  ObjectSideRef staged;
  if (!in.id128(staged.object) || !in.u32(staged.side.value)) {
    return false;
  }
  out = staged;
  return true;
}

void encode_subject_value(Encoder& out, const SubjectValue& value) {
  out.u64(value.object_incarnation.value);
  out.boolean(value.attached);
  encode_port_ref_public(out, value.port);
  out.u32(value.slot.value);
  out.boolean(value.via_move);
  encode_port_ref_public(out, value.move_from);
}

bool decode_subject_value(Decoder& in, SubjectValue& out) {
  SubjectValue staged;
  if (!in.u64(staged.object_incarnation.value) || !in.boolean(staged.attached) ||
      !decode_port_ref(in, staged.port) || !in.u32(staged.slot.value) || !in.boolean(staged.via_move) ||
      !decode_port_ref(in, staged.move_from)) {
    return false;
  }
  if (staged.object_incarnation.value == 0) {
    return false;
  }
  out = staged;
  return true;
}

void encode_port_value(Encoder& out, const PortValue& value) {
  out.u64(value.object_incarnation.value);
  out.boolean(value.empty);
  encode_object_side(out, value.subject);
}

bool decode_port_value(Decoder& in, PortValue& out) {
  PortValue staged;
  if (!in.u64(staged.object_incarnation.value) || !in.boolean(staged.empty) ||
      !decode_object_side(in, staged.subject)) {
    return false;
  }
  out = staged;
  return true;
}

void encode_lifecycle_value(Encoder& out, const LifecycleValue& value, const Limits& limits) {
  out.u64(value.object_incarnation.value);
  out.u8(static_cast<std::uint8_t>(value.action));
  out.string(value.reason, limits.max_string_bytes);
}

bool decode_lifecycle_value(Decoder& in, const Limits& limits, LifecycleValue& out) {
  LifecycleValue staged;
  std::uint8_t action = 0;
  if (!in.u64(staged.object_incarnation.value) || !in.u8(action) ||
      !in.string(staged.reason, limits.max_string_bytes)) {
    return false;
  }
  if (action > static_cast<std::uint8_t>(LifecycleAction::Retired)) {
    return false;
  }
  staged.action = static_cast<LifecycleAction>(action);
  out = std::move(staged);
  return true;
}

void encode_reincarnation_value(Encoder& out, const ReincarnationValue& value, const Limits& limits) {
  out.u64(value.from.value);
  out.u64(value.target.value);
  out.string(value.serial_like, limits.max_string_bytes);
  out.string(value.reason, limits.max_string_bytes);
}

bool decode_reincarnation_value(Decoder& in, const Limits& limits, ReincarnationValue& out) {
  ReincarnationValue staged;
  if (!in.u64(staged.from.value) || !in.u64(staged.target.value) ||
      !in.string(staged.serial_like, limits.max_string_bytes) ||
      !in.string(staged.reason, limits.max_string_bytes)) {
    return false;
  }
  if (staged.from.value == 0 || staged.target.value != staged.from.value + 1) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_object_registration(Encoder& out, const ObjectRegistrationValue& value, const Limits& limits) {
  encode_object_descriptor(out, value.descriptor, limits);
}

bool decode_object_registration(Decoder& in, const Limits& limits, ObjectRegistrationValue& out) {
  return decode_object_descriptor(in, limits, out.descriptor);
}

void encode_endpoint_registration(Encoder& out, const EndpointRegistrationValue& value, const Limits& limits) {
  encode_endpoint_descriptor(out, value.descriptor, limits);
}

bool decode_endpoint_registration(Decoder& in, const Limits& limits, EndpointRegistrationValue& out) {
  return decode_endpoint_descriptor(in, limits, out.descriptor);
}

void encode_object_capability(Encoder& out, const ObjectCapabilityValue& value, const Limits& limits) {
  encode_capability(out, value.capability, limits);
}

bool decode_object_capability(Decoder& in, const Limits& limits, ObjectCapabilityValue& out) {
  return decode_capability(in, limits, out.capability);
}

void encode_port_capability_value(Encoder& out, const PortCapabilityValue& value, const Limits& limits) {
  encode_port_capability(out, value.capability, limits);
}

bool decode_port_capability_value(Decoder& in, const Limits& limits, PortCapabilityValue& out) {
  return decode_port_capability(in, limits, out.capability);
}

void encode_source_record(Encoder& out, const SourceRecord& value, const Limits& limits) {
  encode_source_descriptor(out, value.descriptor, limits);
  out.u64(value.descriptor_incarnation.value);
  out.u64(value.current_incarnation.value);
  out.boolean(value.described);
}

bool decode_source_record(Decoder& in, const Limits& limits, SourceRecord& out) {
  SourceRecord staged;
  if (!decode_source_descriptor(in, limits, staged.descriptor) ||
      !in.u64(staged.descriptor_incarnation.value) || !in.u64(staged.current_incarnation.value) ||
      !in.boolean(staged.described)) {
    return false;
  }
  out = std::move(staged);
  return true;
}

void encode_session_record(Encoder& out, const SessionRecord& value) {
  out.i64(value.opened_at.unix_nanos);
  out.u64(value.high_water.value);
  out.boolean(value.live);
  out.boolean(value.closed);
  out.u64(value.accepted);
  out.u64(value.refused);
  out.u64(value.duplicates);
  out.u64(value.missing_generations);
}

bool decode_session_record(Decoder& in, SessionRecord& out) {
  SessionRecord staged;
  if (!in.i64(staged.opened_at.unix_nanos) || !in.u64(staged.high_water.value) || !in.boolean(staged.live) ||
      !in.boolean(staged.closed) || !in.u64(staged.accepted) || !in.u64(staged.refused) ||
      !in.u64(staged.duplicates) || !in.u64(staged.missing_generations)) {
    return false;
  }
  out = staged;
  return true;
}

/// Encodes one "primary key -> owner -> folded record" group.
template <class PrimaryKey, class T, class EncodePrimary, class EncodeValue>
void encode_group(Encoder& out,
                  const std::map<PrimaryKey, std::map<SourceIncarnationKey, Folded<T>>>& groups,
                  EncodePrimary encode_primary,
                  EncodeValue encode_value,
                  const Limits& limits) {
  out.u32(static_cast<std::uint32_t>(groups.size()));
  for (const auto& entry : groups) {
    encode_primary(out, entry.first);
    out.u32(static_cast<std::uint32_t>(entry.second.size()));
    for (const auto& folded : entry.second) {
      encode_owner(out, folded.first);
      encode_folded(out, folded.second, encode_value, limits);
    }
  }
}

template <class PrimaryKey, class T, class DecodePrimary, class DecodeValue>
bool decode_group(Decoder& in,
                  const Limits& limits,
                  std::uint32_t max_primary,
                  std::uint32_t max_owners,
                  std::map<PrimaryKey, std::map<SourceIncarnationKey, Folded<T>>>& out,
                  DecodePrimary decode_primary,
                  DecodeValue decode_value) {
  std::uint32_t primary_count = 0;
  if (!in.u32(primary_count) || primary_count > max_primary) {
    return false;
  }
  std::map<PrimaryKey, std::map<SourceIncarnationKey, Folded<T>>> staged;
  for (std::uint32_t primary_index = 0; primary_index < primary_count; ++primary_index) {
    PrimaryKey key{};
    if (!decode_primary(key)) {
      return false;
    }
    std::uint32_t owner_count = 0;
    if (!in.u32(owner_count) || owner_count > max_owners) {
      return false;
    }
    auto& owners = staged[key];
    for (std::uint32_t owner_index = 0; owner_index < owner_count; ++owner_index) {
      SourceIncarnationKey owner{};
      if (!decode_owner(in, owner)) {
        return false;
      }
      Folded<T> folded;
      if (!decode_folded(in, limits, limits.max_history_per_key, folded, decode_value)) {
        return false;
      }
      owners.emplace(owner, std::move(folded));
    }
  }
  out = std::move(staged);
  return true;
}

} // namespace

ObjectIncarnation object_incarnation_of(const RegistryState& state, const ObjectId& object) {
  ObjectIncarnation current{1};
  const auto found = state.reincarnation_records.find(object);
  if (found == state.reincarnation_records.end()) {
    return current;
  }
  for (const auto& entry : found->second) {
    if (session_is_fenced(state, entry.first)) {
      continue;
    }
    if (entry.second.value.target > current) {
      current = entry.second.value.target;
    }
  }
  return current;
}

std::uint32_t endpoint_port_count(const RegistryState& state, const EndpointId& endpoint) {
  const auto found = state.endpoint_registrations.find(endpoint);
  if (found == state.endpoint_registrations.end() || found->second.empty()) {
    return 0;
  }
  // The port count is a catalogue fact: it survives the fencing of the source
  // incarnation that declared it.
  std::uint32_t count = 0;
  for (const auto& entry : found->second) {
    if (entry.second.value.descriptor.port_count > count) {
      count = entry.second.value.descriptor.port_count;
    }
  }
  return count;
}

void encode_state(Encoder& out, const RegistryState& state, const Limits& limits) {
  out.u32(kStoreFormatVersion);
  out.id128(state.id.value());
  out.i64(state.opened_at.unix_nanos);

  out.u32(static_cast<std::uint32_t>(state.sources.size()));
  for (const auto& entry : state.sources) {
    out.id128(entry.first.value());
    encode_source_record(out, entry.second, limits);
  }

  out.u32(static_cast<std::uint32_t>(state.sessions.size()));
  for (const auto& entry : state.sessions) {
    encode_owner(out, entry.first);
    encode_session_record(out, entry.second);
  }

  encode_group(
      out,
      state.object_registrations,
      [](Encoder& encoder, const ObjectId& key) { encode_object_id(encoder, key); },
      [&](Encoder& encoder, const ObjectRegistrationValue& value) {
        encode_object_registration(encoder, value, limits);
      },
      limits);
  encode_group(
      out,
      state.endpoint_registrations,
      [](Encoder& encoder, const EndpointId& key) { encode_endpoint_id(encoder, key); },
      [&](Encoder& encoder, const EndpointRegistrationValue& value) {
        encode_endpoint_registration(encoder, value, limits);
      },
      limits);
  encode_group(
      out,
      state.subject_records,
      [](Encoder& encoder, const ObjectSideRef& key) { encode_object_side(encoder, key); },
      [](Encoder& encoder, const SubjectValue& value) { encode_subject_value(encoder, value); },
      limits);
  // Port records are keyed by (source incarnation, slot) inside the port, so
  // they are written by a dedicated pass rather than the generic group helper.
  out.u32(static_cast<std::uint32_t>(state.port_records.size()));
  for (const auto& port_entry : state.port_records) {
    encode_port_ref_public(out, port_entry.first);
    out.u32(static_cast<std::uint32_t>(port_entry.second.size()));
    for (const auto& slot_entry : port_entry.second) {
      encode_owner(out, slot_entry.first.owner);
      out.u32(slot_entry.first.slot.value);
      encode_folded(
          out, slot_entry.second, [](Encoder& encoder, const PortValue& value) { encode_port_value(encoder, value); }, limits);
    }
  }
  encode_group(
      out,
      state.lifecycle_records,
      [](Encoder& encoder, const ObjectId& key) { encode_object_id(encoder, key); },
      [&](Encoder& encoder, const LifecycleValue& value) { encode_lifecycle_value(encoder, value, limits); },
      limits);
  encode_group(
      out,
      state.reincarnation_records,
      [](Encoder& encoder, const ObjectId& key) { encode_object_id(encoder, key); },
      [&](Encoder& encoder, const ReincarnationValue& value) {
        encode_reincarnation_value(encoder, value, limits);
      },
      limits);
  encode_group(
      out,
      state.object_capability_records,
      [](Encoder& encoder, const ObjectId& key) { encode_object_id(encoder, key); },
      [&](Encoder& encoder, const ObjectCapabilityValue& value) {
        encode_object_capability(encoder, value, limits);
      },
      limits);
  encode_group(
      out,
      state.port_capability_records,
      [](Encoder& encoder, const PortRef& key) { encode_port_ref_public(encoder, key); },
      [&](Encoder& encoder, const PortCapabilityValue& value) {
        encode_port_capability_value(encoder, value, limits);
      },
      limits);
}

bool decode_state(Decoder& in, const Limits& limits, RegistryState& out) {
  std::uint32_t format = 0;
  if (!in.u32(format) || format != kStoreFormatVersion) {
    return false;
  }
  RegistryState staged;
  if (!in.id128(staged.id) || !in.i64(staged.opened_at.unix_nanos)) {
    return false;
  }

  std::uint32_t source_count = 0;
  if (!in.u32(source_count) || source_count > limits.max_sources) {
    return false;
  }
  for (std::uint32_t index = 0; index < source_count; ++index) {
    SourceId id;
    SourceRecord record;
    if (!in.id128(id) || !decode_source_record(in, limits, record)) {
      return false;
    }
    staged.sources.emplace(id, std::move(record));
  }

  std::uint32_t session_count = 0;
  if (!in.u32(session_count) || session_count > limits.max_sources * 64u) {
    return false;
  }
  for (std::uint32_t index = 0; index < session_count; ++index) {
    SourceIncarnationKey owner;
    SessionRecord record;
    if (!decode_owner(in, owner) || !decode_session_record(in, record)) {
      return false;
    }
    staged.sessions.emplace(owner, record);
  }

  if (!decode_group<ObjectId, ObjectRegistrationValue>(
          in,
          limits,
          limits.max_objects,
          limits.max_sources,
          staged.object_registrations,
          [&](ObjectId& key) { return in.id128(key); },
          [&](ObjectRegistrationValue& value) { return decode_object_registration(in, limits, value); })) {
    return false;
  }
  if (!decode_group<EndpointId, EndpointRegistrationValue>(
          in,
          limits,
          limits.max_endpoints,
          limits.max_sources,
          staged.endpoint_registrations,
          [&](EndpointId& key) { return in.id128(key); },
          [&](EndpointRegistrationValue& value) { return decode_endpoint_registration(in, limits, value); })) {
    return false;
  }
  if (!decode_group<ObjectSideRef, SubjectValue>(
          in,
          limits,
          limits.max_objects * limits.max_sides_per_object,
          limits.max_sources,
          staged.subject_records,
          [&](ObjectSideRef& key) { return decode_object_side(in, key); },
          [&](SubjectValue& value) { return decode_subject_value(in, value); })) {
    return false;
  }
  std::uint32_t port_record_count = 0;
  if (!in.u32(port_record_count) ||
      port_record_count > limits.max_endpoints * limits.max_ports_per_endpoint) {
    return false;
  }
  for (std::uint32_t port_index = 0; port_index < port_record_count; ++port_index) {
    PortRef port;
    if (!decode_port_ref(in, port)) {
      return false;
    }
    std::uint32_t slot_count = 0;
    if (!in.u32(slot_count) || slot_count > limits.max_sources * limits.max_port_simultaneous_attachments) {
      return false;
    }
    auto& slots = staged.port_records[port];
    for (std::uint32_t slot_index = 0; slot_index < slot_count; ++slot_index) {
      PortSlotKey key;
      key.port = port;
      if (!decode_owner(in, key.owner) || !in.u32(key.slot.value)) {
        return false;
      }
      Folded<PortValue> folded;
      if (!decode_folded(in, limits, limits.max_history_per_key, folded, [&](PortValue& value) {
            return decode_port_value(in, value);
          })) {
        return false;
      }
      slots.emplace(key, std::move(folded));
    }
  }
  if (!decode_group<ObjectId, LifecycleValue>(
          in,
          limits,
          limits.max_objects,
          limits.max_sources,
          staged.lifecycle_records,
          [&](ObjectId& key) { return in.id128(key); },
          [&](LifecycleValue& value) { return decode_lifecycle_value(in, limits, value); })) {
    return false;
  }
  if (!decode_group<ObjectId, ReincarnationValue>(
          in,
          limits,
          limits.max_objects,
          limits.max_sources,
          staged.reincarnation_records,
          [&](ObjectId& key) { return in.id128(key); },
          [&](ReincarnationValue& value) { return decode_reincarnation_value(in, limits, value); })) {
    return false;
  }
  if (!decode_group<ObjectId, ObjectCapabilityValue>(
          in,
          limits,
          limits.max_objects,
          limits.max_sources,
          staged.object_capability_records,
          [&](ObjectId& key) { return in.id128(key); },
          [&](ObjectCapabilityValue& value) { return decode_object_capability(in, limits, value); })) {
    return false;
  }
  if (!decode_group<PortRef, PortCapabilityValue>(
          in,
          limits,
          limits.max_endpoints * limits.max_ports_per_endpoint,
          limits.max_sources,
          staged.port_capability_records,
          [&](PortRef& key) { return decode_port_ref(in, key); },
          [&](PortCapabilityValue& value) { return decode_port_capability_value(in, limits, value); })) {
    return false;
  }

  out = std::move(staged);
  return true;
}

void clear_volatile(RegistryState& state) {
  for (auto& entry : state.sessions) {
    entry.second.live = false;
  }
  auto clear_group = [](auto& groups) {
    for (auto& group : groups) {
      for (auto& owner : group.second) {
        owner.second.meta.asserted_in_lifetime = false;
        for (Attestation& attestation : owner.second.superseded) {
          attestation.asserted_in_lifetime = false;
        }
      }
    }
  };
  clear_group(state.object_registrations);
  clear_group(state.endpoint_registrations);
  clear_group(state.subject_records);
  clear_group(state.lifecycle_records);
  clear_group(state.reincarnation_records);
  clear_group(state.object_capability_records);
  clear_group(state.port_capability_records);
  for (auto& port : state.port_records) {
    for (auto& owner : port.second) {
      owner.second.meta.asserted_in_lifetime = false;
      for (Attestation& attestation : owner.second.superseded) {
        attestation.asserted_in_lifetime = false;
      }
    }
  }
}

namespace {

void image_attestation(Encoder& out, const Attestation& value) {
  encode_owner(out, value.owner);
  out.u64(value.generation.value);
  out.id128(value.evidence.value());
  out.i64(value.observed_at.unix_nanos);
  out.u8(static_cast<std::uint8_t>(value.provenance));
  out.u8(static_cast<std::uint8_t>(value.authority.authority_class));
  out.u32(value.authority.rank);
}

template <class PrimaryKey, class T, class EncodePrimary, class EncodeValue>
void image_group(Encoder& out,
                 const std::map<PrimaryKey, std::map<SourceIncarnationKey, Folded<T>>>& groups,
                 EncodePrimary encode_primary,
                 EncodeValue encode_value,
                 const Limits& limits) {
  out.u32(static_cast<std::uint32_t>(groups.size()));
  for (const auto& entry : groups) {
    encode_primary(out, entry.first);
    out.u32(static_cast<std::uint32_t>(entry.second.size()));
    for (const auto& folded : entry.second) {
      image_attestation(out, folded.second.meta);
      encode_value(out, folded.second.value);
    }
  }
  (void)limits;
}

} // namespace

void encode_provenance_image(Encoder& out, const RegistryState& state, const Limits& limits) {
  out.u32(kStoreFormatVersion);
  image_group(
      out,
      state.object_registrations,
      [](Encoder& encoder, const ObjectId& key) { encoder.id128(key.value()); },
      [&](Encoder& encoder, const ObjectRegistrationValue& value) {
        encode_object_registration(encoder, value, limits);
      },
      limits);
  image_group(
      out,
      state.endpoint_registrations,
      [](Encoder& encoder, const EndpointId& key) { encoder.id128(key.value()); },
      [&](Encoder& encoder, const EndpointRegistrationValue& value) {
        encode_endpoint_registration(encoder, value, limits);
      },
      limits);
  image_group(
      out,
      state.subject_records,
      [](Encoder& encoder, const ObjectSideRef& key) {
        encoder.id128(key.object.value());
        encoder.u32(key.side.value);
      },
      [](Encoder& encoder, const SubjectValue& value) { encode_subject_value(encoder, value); },
      limits);
  out.u32(static_cast<std::uint32_t>(state.port_records.size()));
  for (const auto& port : state.port_records) {
    encode_port_ref_public(out, port.first);
    out.u32(static_cast<std::uint32_t>(port.second.size()));
    for (const auto& folded : port.second) {
      image_attestation(out, folded.second.meta);
      encode_owner(out, folded.first.owner);
      out.u32(folded.first.slot.value);
      encode_port_value(out, folded.second.value);
    }
  }
  image_group(
      out,
      state.lifecycle_records,
      [](Encoder& encoder, const ObjectId& key) { encoder.id128(key.value()); },
      [&](Encoder& encoder, const LifecycleValue& value) { encode_lifecycle_value(encoder, value, limits); },
      limits);
  image_group(
      out,
      state.reincarnation_records,
      [](Encoder& encoder, const ObjectId& key) { encoder.id128(key.value()); },
      [&](Encoder& encoder, const ReincarnationValue& value) {
        encode_reincarnation_value(encoder, value, limits);
      },
      limits);
  image_group(
      out,
      state.object_capability_records,
      [](Encoder& encoder, const ObjectId& key) { encoder.id128(key.value()); },
      [&](Encoder& encoder, const ObjectCapabilityValue& value) {
        encode_object_capability(encoder, value, limits);
      },
      limits);
  image_group(
      out,
      state.port_capability_records,
      [](Encoder& encoder, const PortRef& key) { encode_port_ref_public(encoder, key); },
      [&](Encoder& encoder, const PortCapabilityValue& value) {
        encode_port_capability_value(encoder, value, limits);
      },
      limits);
}

} // namespace cable_registry::detail
