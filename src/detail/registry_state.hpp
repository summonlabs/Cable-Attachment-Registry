// Cable Attachment Registry — internal registry state.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal header. The state is a set of folded records: for every claim key
// the registry keeps the record with the highest generation of that source
// incarnation. Nothing here depends on the order records arrived in, which is
// what makes the canonical graph arrival-order independent.

#ifndef CABLE_REGISTRY_DETAIL_REGISTRY_STATE_HPP
#define CABLE_REGISTRY_DETAIL_REGISTRY_STATE_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cable_registry/authority.hpp"
#include "cable_registry/codec.hpp"
#include "cable_registry/evidence.hpp"
#include "cable_registry/identity.hpp"
#include "cable_registry/ids.hpp"
#include "cable_registry/limits.hpp"
#include "cable_registry/time.hpp"

namespace cable_registry::detail {

/// Everything the registry knows about how one record was produced.
struct Attestation {
  SourceIncarnationKey owner{};
  Generation generation{};
  EvidenceId evidence{};
  Timestamp observed_at{};
  Timestamp received_at{};
  ProvenanceClass provenance = ProvenanceClass::Unknown;
  Authority authority{};
  /// Incarnation of the physical object the record was made against, when the
  /// evidence kind carries one. Zero means "not applicable".
  ObjectIncarnation object_incarnation{};
  /// True when the record was accepted or re-asserted while its source session
  /// was live in this process. Recovered records are false until renewed.
  bool asserted_in_lifetime = false;
};

/// A claim key's winning record plus a bounded, generation ordered ring of the
/// records it superseded. The ring keeps the highest generations, so its
/// contents are a function of the record set rather than its arrival order.
template <class T>
struct Folded {
  Attestation meta;
  T value{};
  std::vector<Attestation> superseded;
  std::uint32_t history_dropped = 0;
};

struct SourceRecord {
  SourceDescriptor descriptor;
  Incarnation descriptor_incarnation{};
  Incarnation current_incarnation{};
  bool described = false;
};

struct SessionRecord {
  Timestamp opened_at{};
  Generation high_water{};
  bool live = false;
  bool closed = false;
  std::uint64_t accepted = 0;
  std::uint64_t refused = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t missing_generations = 0;
};

/// One object side's position, as asserted by one source incarnation.
struct SubjectValue {
  ObjectIncarnation object_incarnation{};
  bool attached = false;
  PortRef port{};
  AttachmentSlot slot{};
  /// The record was a Move, and from_port names where the side came from.
  bool via_move = false;
  PortRef move_from{};

  friend bool operator==(const SubjectValue&, const SubjectValue&) noexcept = default;
};

/// One attachment slot of one port, as asserted by one source incarnation.
struct PortValue {
  ObjectIncarnation object_incarnation{};
  /// True asserts the slot is empty.
  bool empty = true;
  /// Meaningful when empty is false.
  ObjectSideRef subject{};

  friend bool operator==(const PortValue&, const PortValue&) noexcept = default;
};

struct LifecycleValue {
  ObjectIncarnation object_incarnation{};
  LifecycleAction action = LifecycleAction::Present;
  std::string reason;

  friend bool operator==(const LifecycleValue&, const LifecycleValue&) noexcept = default;
};

struct ReincarnationValue {
  ObjectIncarnation from{};
  ObjectIncarnation target{};
  std::string serial_like;
  std::string reason;

  friend bool operator==(const ReincarnationValue&, const ReincarnationValue&) noexcept = default;
};

struct ObjectCapabilityValue {
  NominalCapability capability;
};

struct PortCapabilityValue {
  PortCapability capability;
};

struct ObjectRegistrationValue {
  ObjectDescriptor descriptor;
};

struct EndpointRegistrationValue {
  EndpointDescriptor descriptor;
};

/// The key of one attachment slot record: which source incarnation asserts it,
/// and which slot of the port it is about.
struct PortSlotKey {
  SourceIncarnationKey owner{};
  /// Carried for readability at the call sites; it always equals the outer map
  /// key of the port map the record lives in.
  PortRef port{};
  AttachmentSlot slot{};

  friend bool operator==(const PortSlotKey&, const PortSlotKey&) noexcept = default;
  friend auto operator<=>(const PortSlotKey&, const PortSlotKey&) noexcept = default;
};

/// The whole registry state. Every map is ordered, so iteration is
/// deterministic without a separate sort.
struct RegistryState {
  RegistryId id{};
  Timestamp opened_at{};
  std::map<SourceId, SourceRecord> sources;
  std::map<SourceIncarnationKey, SessionRecord> sessions;
  std::map<ObjectId, std::map<SourceIncarnationKey, Folded<ObjectRegistrationValue>>> object_registrations;
  std::map<EndpointId, std::map<SourceIncarnationKey, Folded<EndpointRegistrationValue>>> endpoint_registrations;
  std::map<ObjectSideRef, std::map<SourceIncarnationKey, Folded<SubjectValue>>> subject_records;
  std::map<PortRef, std::map<PortSlotKey, Folded<PortValue>>> port_records;
  std::map<ObjectId, std::map<SourceIncarnationKey, Folded<LifecycleValue>>> lifecycle_records;
  std::map<ObjectId, std::map<SourceIncarnationKey, Folded<ReincarnationValue>>> reincarnation_records;
  std::map<ObjectId, std::map<SourceIncarnationKey, Folded<ObjectCapabilityValue>>> object_capability_records;
  std::map<PortRef, std::map<SourceIncarnationKey, Folded<PortCapabilityValue>>> port_capability_records;
};

/// Session fencing: a session is fenced once a strictly higher incarnation of
/// the same source exists.
[[nodiscard]] inline bool session_is_fenced(const RegistryState& state, const SourceIncarnationKey& key) {
  const auto source = state.sources.find(key.source);
  if (source == state.sources.end()) {
    return false;
  }
  return source->second.current_incarnation > key.incarnation;
}

/// The canonical incarnation of a physical object: one plus the highest target
/// asserted by a non-fenced reincarnation record.
[[nodiscard]] ObjectIncarnation object_incarnation_of(const RegistryState& state, const ObjectId& object);

/// True when the object has at least one registration record.
[[nodiscard]] inline bool object_exists(const RegistryState& state, const ObjectId& object) {
  const auto found = state.object_registrations.find(object);
  return found != state.object_registrations.end() && !found->second.empty();
}

[[nodiscard]] inline bool endpoint_exists(const RegistryState& state, const EndpointId& endpoint) {
  const auto found = state.endpoint_registrations.find(endpoint);
  return found != state.endpoint_registrations.end() && !found->second.empty();
}

/// The port count of a registered endpoint, or zero when it is not registered.
[[nodiscard]] std::uint32_t endpoint_port_count(const RegistryState& state, const EndpointId& endpoint);

/// True when the port reference names a port that exists.
[[nodiscard]] inline bool port_exists(const RegistryState& state, const PortRef& port) {
  return port.index.value < endpoint_port_count(state, port.endpoint);
}

/// Canonical encoding of the whole state, used by log compaction and by the
/// recovery path that loads a compacted image.
void encode_state(Encoder& out, const RegistryState& state, const Limits& limits);
[[nodiscard]] bool decode_state(Decoder& in, const Limits& limits, RegistryState& out);

/// Resets everything that is a property of this process rather than of the
/// evidence: session liveness and per-record lifetime validation. A recovered
/// store never claims that its records are fresh.
void clear_volatile(RegistryState& state);

/// Canonical image of the accepted evidence set. It contains only fields that
/// are functions of the evidence: no receive timestamps, no liveness, no
/// lifetime flags, no registry identity. Two registries that accepted the same
/// records produce the same bytes whatever order the records arrived in.
void encode_provenance_image(Encoder& out, const RegistryState& state, const Limits& limits);

} // namespace cable_registry::detail


#endif // CABLE_REGISTRY_DETAIL_REGISTRY_STATE_HPP
