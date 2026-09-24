// Cable Attachment Registry — the registry runtime.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The runtime is a fold over accepted evidence. Every claim key keeps the
// record with the highest generation of its source incarnation; disagreement
// across sources is resolved by Authority and, when authority ties, is
// reported rather than resolved. Nothing in the derivation reads a receive
// timestamp or an arrival position, which is what makes the canonical graph a
// pure function of the accepted evidence set.

#include "cable_registry/registry.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "cable_registry/codec.hpp"
#include "cable_registry/digest.hpp"
#include "cable_registry/evidence.hpp"
#include "cable_registry/serialization.hpp"
#include "cable_registry/version.hpp"
#include "detail/registry_state.hpp"

namespace cable_registry {
namespace {

using detail::Attestation;
using detail::EndpointRegistrationValue;
using detail::Folded;
using detail::LifecycleValue;
using detail::ObjectCapabilityValue;
using detail::ObjectRegistrationValue;
using detail::PortCapabilityValue;
using detail::PortSlotKey;
using detail::PortValue;
using detail::RegistryState;
using detail::ReincarnationValue;
using detail::SessionRecord;
using detail::SourceRecord;
using detail::SubjectValue;

constexpr std::uint64_t kMaxObjectIncarnation = 1ull << 32;

bool fits(std::string_view text, std::uint32_t max_bytes) noexcept {
  return text.size() <= max_bytes;
}

bool validate_object_descriptor(const ObjectDescriptor& descriptor, const Limits& limits, std::string& error) {
  if (descriptor.id.is_nil()) {
    error = "object identity is nil";
    return false;
  }
  if (descriptor.sides.empty()) {
    error = "an object must declare at least one side";
    return false;
  }
  if (descriptor.sides.size() > limits.max_sides_per_object) {
    error = "an object declares more sides than the configured limit";
    return false;
  }
  if (!fits(descriptor.physical_label, limits.max_string_bytes) ||
      !fits(descriptor.serial_like, limits.max_string_bytes) ||
      !fits(descriptor.administrative_location, limits.max_string_bytes)) {
    error = "an object descriptor string exceeds the configured limit";
    return false;
  }
  if (!fits(descriptor.capability.capability_code, limits.max_capability_code_bytes)) {
    error = "the capability code exceeds the configured limit";
    return false;
  }
  if (descriptor.kind > ObjectKind::Other) {
    error = "the object kind is not recognised";
    return false;
  }
  for (const ObjectSideDescriptor& side : descriptor.sides) {
    if (side.connector > ConnectorClass::Other || side.media > MediaClass::Other) {
      error = "a side descriptor uses an unrecognised connector or media class";
      return false;
    }
  }
  if (descriptor.capability.media > MediaClass::Other || descriptor.capability.connector > ConnectorClass::Other) {
    error = "the capability vocabulary is not recognised";
    return false;
  }
  return true;
}

bool validate_endpoint_descriptor(const EndpointDescriptor& descriptor, const Limits& limits, std::string& error) {
  if (descriptor.id.is_nil()) {
    error = "endpoint identity is nil";
    return false;
  }
  if (descriptor.port_count == 0 || descriptor.port_count > limits.max_ports_per_endpoint) {
    error = "the endpoint port count is outside the configured range";
    return false;
  }
  if (!fits(descriptor.name, limits.max_string_bytes) ||
      !fits(descriptor.administrative_location, limits.max_string_bytes)) {
    error = "an endpoint descriptor string exceeds the configured limit";
    return false;
  }
  if (descriptor.kind > EndpointKind::Other) {
    error = "the endpoint kind is not recognised";
    return false;
  }
  if (descriptor.default_port_capability.max_simultaneous_attachments == 0 ||
      descriptor.default_port_capability.max_simultaneous_attachments > limits.max_port_simultaneous_attachments) {
    error = "the default port capability is outside the configured range";
    return false;
  }
  return true;
}

bool validate_evidence_shape(const Evidence& evidence, const Limits& limits, std::string& error) {
  const EvidenceHeader& header = evidence.header;
  if (header.id.is_nil()) {
    error = "evidence identity is nil";
    return false;
  }
  if (header.source.is_nil()) {
    error = "source identity is nil";
    return false;
  }
  if (header.incarnation.value == 0) {
    error = "the source incarnation must be at least one";
    return false;
  }
  if (header.generation.value == 0) {
    error = "the evidence generation must be at least one";
    return false;
  }
  if (header.schema_version != kEvidenceSchemaVersion) {
    error = "unsupported evidence schema version " + std::to_string(header.schema_version);
    return false;
  }
  if (!fits(header.provenance_detail, limits.max_string_bytes)) {
    error = "the provenance detail exceeds the configured limit";
    return false;
  }
  if (header.provenance > ProvenanceClass::Synthetic) {
    error = "the provenance class is not recognised";
    return false;
  }

  switch (kind_of(evidence)) {
    case EvidenceKind::RegisterObject:
      return validate_object_descriptor(std::get<RegisterObjectPayload>(evidence.payload).descriptor, limits, error);
    case EvidenceKind::RegisterEndpoint:
      return validate_endpoint_descriptor(std::get<RegisterEndpointPayload>(evidence.payload).descriptor,
                                          limits,
                                          error);
    case EvidenceKind::Attach: {
      const auto& payload = std::get<AttachPayload>(evidence.payload);
      if (payload.subject.object.is_nil() || payload.port.endpoint.is_nil()) {
        error = "the attachment names a nil identity";
        return false;
      }
      if (payload.object_incarnation.value == 0) {
        error = "the object incarnation must be at least one";
        return false;
      }
      if (payload.slot.value >= limits.max_port_simultaneous_attachments) {
        error = "the attachment slot exceeds the configured limit";
        return false;
      }
      return true;
    }
    case EvidenceKind::Detach: {
      const auto& payload = std::get<DetachPayload>(evidence.payload);
      if (payload.subject.object.is_nil()) {
        error = "the detachment names a nil object";
        return false;
      }
      if (payload.object_incarnation.value == 0) {
        error = "the object incarnation must be at least one";
        return false;
      }
      if (payload.has_from_port && payload.from_port.endpoint.is_nil()) {
        error = "the detachment names a nil port";
        return false;
      }
      if (payload.slot.value >= limits.max_port_simultaneous_attachments) {
        error = "the detachment slot exceeds the configured limit";
        return false;
      }
      return true;
    }
    case EvidenceKind::Move: {
      const auto& payload = std::get<MovePayload>(evidence.payload);
      if (payload.subject.object.is_nil() || payload.from.endpoint.is_nil() || payload.to.endpoint.is_nil()) {
        error = "the move names a nil identity";
        return false;
      }
      if (payload.object_incarnation.value == 0) {
        error = "the object incarnation must be at least one";
        return false;
      }
      if (payload.from_slot.value >= limits.max_port_simultaneous_attachments ||
          payload.to_slot.value >= limits.max_port_simultaneous_attachments) {
        error = "the move slot exceeds the configured limit";
        return false;
      }
      return true;
    }
    case EvidenceKind::PublishObjectCapability: {
      const auto& payload = std::get<PublishObjectCapabilityPayload>(evidence.payload);
      if (payload.object.is_nil()) {
        error = "the capability publication names a nil object";
        return false;
      }
      if (!fits(payload.capability.capability_code, limits.max_capability_code_bytes)) {
        error = "the capability code exceeds the configured limit";
        return false;
      }
      if (payload.capability.media > MediaClass::Other || payload.capability.connector > ConnectorClass::Other) {
        error = "the capability vocabulary is not recognised";
        return false;
      }
      return true;
    }
    case EvidenceKind::PublishPortCapability: {
      const auto& payload = std::get<PublishPortCapabilityPayload>(evidence.payload);
      if (payload.port.endpoint.is_nil()) {
        error = "the capability publication names a nil endpoint";
        return false;
      }
      if (payload.capability.max_simultaneous_attachments == 0 ||
          payload.capability.max_simultaneous_attachments > limits.max_port_simultaneous_attachments) {
        error = "the port capability is outside the configured range";
        return false;
      }
      return true;
    }
    case EvidenceKind::SetLifecycle: {
      const auto& payload = std::get<SetLifecyclePayload>(evidence.payload);
      if (payload.object.is_nil()) {
        error = "the lifecycle assertion names a nil object";
        return false;
      }
      if (payload.object_incarnation.value == 0) {
        error = "the object incarnation must be at least one";
        return false;
      }
      if (payload.action > LifecycleAction::Retired) {
        error = "the lifecycle action is not recognised";
        return false;
      }
      if (!fits(payload.reason, limits.max_string_bytes)) {
        error = "the lifecycle reason exceeds the configured limit";
        return false;
      }
      return true;
    }
    case EvidenceKind::ReincarnateObject: {
      const auto& payload = std::get<ReincarnateObjectPayload>(evidence.payload);
      if (payload.object.is_nil()) {
        error = "the reincarnation names a nil object";
        return false;
      }
      if (payload.from_incarnation.value == 0 || payload.from_incarnation.value + 1 > kMaxObjectIncarnation) {
        error = "the reincarnation source incarnation is outside the supported range";
        return false;
      }
      if (!fits(payload.new_serial_like, limits.max_string_bytes) ||
          !fits(payload.reason, limits.max_string_bytes)) {
        error = "a reincarnation string exceeds the configured limit";
        return false;
      }
      return true;
    }
  }
  error = "the evidence kind is not recognised";
  return false;
}

enum class GenerationOutcome { Insert, Superseded, Duplicate, Collision };

GenerationOutcome compare_generation(const Attestation& stored,
                                     const Attestation& incoming,
                                     bool same_content) noexcept {
  if (incoming.generation > stored.generation) {
    return GenerationOutcome::Insert;
  }
  if (incoming.generation < stored.generation) {
    return GenerationOutcome::Superseded;
  }
  return same_content ? GenerationOutcome::Duplicate : GenerationOutcome::Collision;
}

ValidationState combine_validation(bool any, bool all) noexcept {
  if (!any) {
    return ValidationState::Unvalidated;
  }
  return all ? ValidationState::Validated : ValidationState::PartiallyValidated;
}

ClaimProvenance to_provenance(const Attestation& attestation) {
  ClaimProvenance provenance;
  provenance.source = attestation.owner.source;
  provenance.incarnation = attestation.owner.incarnation;
  provenance.generation = attestation.generation;
  provenance.evidence = attestation.evidence;
  provenance.observed_at = attestation.observed_at;
  provenance.received_at = attestation.received_at;
  provenance.provenance = attestation.provenance;
  provenance.authority = attestation.authority;
  provenance.object_incarnation = attestation.object_incarnation;
  provenance.asserted_in_lifetime = attestation.asserted_in_lifetime;
  return provenance;
}

bool claim_order(const ClaimProvenance& left, const ClaimProvenance& right) {
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

/// The physical incarnation an evidence record was made against, when its kind
/// carries one.
ObjectIncarnation object_incarnation_of_evidence(const Evidence& evidence) noexcept {
  switch (kind_of(evidence)) {
    case EvidenceKind::Attach:
      return std::get<AttachPayload>(evidence.payload).object_incarnation;
    case EvidenceKind::Detach:
      return std::get<DetachPayload>(evidence.payload).object_incarnation;
    case EvidenceKind::Move:
      return std::get<MovePayload>(evidence.payload).object_incarnation;
    case EvidenceKind::SetLifecycle:
      return std::get<SetLifecyclePayload>(evidence.payload).object_incarnation;
    default:
      return ObjectIncarnation{};
  }
}

bool participates(const Attestation& meta, ValidationPolicy policy) noexcept {
  return policy == ValidationPolicy::IncludeAll || meta.asserted_in_lifetime;
}

/// The winning and losing claims for one key.
struct ClaimSet {
  bool any = false;
  Authority authority{};
  ValidationState validation = ValidationState::Unvalidated;
  std::vector<Attestation> top;
  std::vector<Attestation> lower;
  std::uint32_t lower_total = 0;
};

template <class T, class Accept>
ClaimSet collect_claims(const std::map<SourceIncarnationKey, Folded<T>>& records,
                        ValidationPolicy policy,
                        Accept accept) {
  ClaimSet set;
  std::vector<const Folded<T>*> eligible;
  for (const auto& entry : records) {
    if (!participates(entry.second.meta, policy)) {
      continue;
    }
    if (!accept(entry.first, entry.second)) {
      continue;
    }
    eligible.push_back(&entry.second);
  }
  if (eligible.empty()) {
    return set;
  }
  set.any = true;
  set.authority = eligible.front()->meta.authority;
  for (const Folded<T>* record : eligible) {
    if (set.authority < record->meta.authority) {
      set.authority = record->meta.authority;
    }
  }
  bool any_lifetime = false;
  bool all_lifetime = true;
  for (const Folded<T>* record : eligible) {
    any_lifetime = any_lifetime || record->meta.asserted_in_lifetime;
    all_lifetime = all_lifetime && record->meta.asserted_in_lifetime;
    if (record->meta.authority == set.authority) {
      set.top.push_back(record->meta);
    } else {
      ++set.lower_total;
      set.lower.push_back(record->meta);
    }
  }
  set.validation = combine_validation(any_lifetime, all_lifetime);
  std::sort(set.top.begin(), set.top.end(), [](const Attestation& left, const Attestation& right) {
    if (left.owner != right.owner) {
      return left.owner < right.owner;
    }
    return left.generation < right.generation;
  });
  std::sort(set.lower.begin(), set.lower.end(), [](const Attestation& left, const Attestation& right) {
    if (!(left.authority == right.authority)) {
      return right.authority < left.authority;
    }
    if (left.owner != right.owner) {
      return left.owner < right.owner;
    }
    return left.generation < right.generation;
  });
  return set;
}

// -- derivation helpers -----------------------------------------------------

const Folded<ObjectRegistrationValue>* winning_registration(const RegistryState& state, const ObjectId& object) {
  const auto found = state.object_registrations.find(object);
  if (found == state.object_registrations.end()) {
    return nullptr;
  }
  // An object or endpoint registration is a catalogue fact about the plant,
  // not an observation of its current state, so it survives the fencing of the
  // source incarnation that contributed it. Attachment, lifecycle and
  // capability claims are fenced as usual.
  const Folded<ObjectRegistrationValue>* chosen = nullptr;
  SourceIncarnationKey chosen_key{};
  for (const auto& entry : found->second) {
    if (chosen == nullptr || chosen->meta.authority < entry.second.meta.authority ||
        (chosen->meta.authority == entry.second.meta.authority && entry.first < chosen_key)) {
      chosen = &entry.second;
      chosen_key = entry.first;
    }
  }
  return chosen;
}

const Folded<EndpointRegistrationValue>* winning_endpoint(const RegistryState& state, const EndpointId& endpoint) {
  const auto found = state.endpoint_registrations.find(endpoint);
  if (found == state.endpoint_registrations.end()) {
    return nullptr;
  }
  const Folded<EndpointRegistrationValue>* chosen = nullptr;
  SourceIncarnationKey chosen_key{};
  for (const auto& entry : found->second) {
    if (chosen == nullptr || chosen->meta.authority < entry.second.meta.authority ||
        (chosen->meta.authority == entry.second.meta.authority && entry.first < chosen_key)) {
      chosen = &entry.second;
      chosen_key = entry.first;
    }
  }
  return chosen;
}

PortCapability port_capability_of(const RegistryState& state, const PortRef& port) {
  const auto published = state.port_capability_records.find(port);
  if (published != state.port_capability_records.end()) {
    const Folded<PortCapabilityValue>* chosen = nullptr;
    SourceIncarnationKey chosen_key{};
    Authority best{};
    std::uint32_t minimum = 0;
    for (const auto& entry : published->second) {
      if (detail::session_is_fenced(state, entry.first)) {
        continue;
      }
      const Attestation& meta = entry.second.meta;
      if (chosen == nullptr || best < meta.authority) {
        best = meta.authority;
        chosen = &entry.second;
        chosen_key = entry.first;
        minimum = entry.second.value.capability.max_simultaneous_attachments;
      } else if (meta.authority == best) {
        // Equal authority declarations that disagree are resolved
        // conservatively: the smallest admitted attachment count wins, so the
        // registry never invents capacity that one source denies.
        minimum = std::min(minimum, entry.second.value.capability.max_simultaneous_attachments);
        if (entry.first < chosen_key) {
          chosen = &entry.second;
          chosen_key = entry.first;
        }
      }
    }
    if (chosen != nullptr) {
      PortCapability capability = chosen->value.capability;
      capability.max_simultaneous_attachments = minimum == 0 ? 1 : minimum;
      return capability;
    }
  }
  const Folded<EndpointRegistrationValue>* endpoint = winning_endpoint(state, port.endpoint);
  if (endpoint != nullptr) {
    return endpoint->value.descriptor.default_port_capability;
  }
  return PortCapability{};
}

ObjectSideDescriptor side_descriptor_of(const RegistryState& state, const ObjectId& object, const SideIndex& side) {
  const Folded<ObjectRegistrationValue>* registration = winning_registration(state, object);
  if (registration == nullptr || side.value >= registration->value.descriptor.sides.size()) {
    return ObjectSideDescriptor{};
  }
  return registration->value.descriptor.sides[side.value];
}

ObjectIncarnation subject_incarnation_of(const SubjectValue& value) noexcept {
  return value.object_incarnation;
}

/// Derives one object side from its own records alone.
struct SubjectDerivation {
  bool has_records = false;
  PortAttachmentState state = PortAttachmentState::Unknown;
  ValidationState validation = ValidationState::Unvalidated;
  Authority authority{};
  ObjectIncarnation incarnation{1};
  std::vector<PortRef> ports;
  std::vector<AttachmentSlot> slots;
  std::vector<Attestation> top;
};

/// Accept predicate shared by every consumer of subject records: the session
/// must not be fenced and the claim must be against the object's current
/// incarnation.
template <class T>
bool subject_record_live(const RegistryState& state, const SourceIncarnationKey& owner, const Folded<T>& record,
                         ValidationPolicy policy) {
  if (detail::session_is_fenced(state, owner)) {
    return false;
  }
  if (!participates(record.meta, policy)) {
    return false;
  }
  return true;
}

SubjectDerivation derive_subject(const RegistryState& state,
                                 const ObjectSideRef& subject,
                                 ValidationPolicy policy) {
  SubjectDerivation derivation;
  derivation.incarnation = detail::object_incarnation_of(state, subject.object);
  if (!detail::object_exists(state, subject.object)) {
    return derivation;
  }
  const auto found = state.subject_records.find(subject);
  if (found == state.subject_records.end()) {
    return derivation;
  }

  const ObjectIncarnation current = derivation.incarnation;
  ClaimSet attached = collect_claims<SubjectValue>(found->second, policy,
                                                   [&](const SourceIncarnationKey& owner,
                                                       const Folded<SubjectValue>& record) {
                                                     if (!subject_record_live(state, owner, record, policy)) {
                                                       return false;
                                                     }
                                                     return record.value.object_incarnation == current &&
                                                            record.value.attached;
                                                   });
  ClaimSet detached = collect_claims<SubjectValue>(found->second, policy,
                                                   [&](const SourceIncarnationKey& owner,
                                                       const Folded<SubjectValue>& record) {
                                                     if (!subject_record_live(state, owner, record, policy)) {
                                                       return false;
                                                     }
                                                     return record.value.object_incarnation == current &&
                                                            !record.value.attached;
                                                   });

  if (!attached.any && !detached.any) {
    return derivation;
  }
  derivation.has_records = true;

  const bool attached_wins = attached.any && (!detached.any || detached.authority < attached.authority);
  const bool detached_wins = detached.any && (!attached.any || attached.authority < detached.authority);
  if (!attached_wins && !detached_wins) {
    derivation.state = PortAttachmentState::Conflicting;
    derivation.authority = attached.any ? attached.authority : detached.authority;
    derivation.validation = combine_validation(
        (attached.any && attached.validation != ValidationState::Unvalidated) ||
            (detached.any && detached.validation != ValidationState::Unvalidated),
        attached.validation == ValidationState::Validated || detached.validation == ValidationState::Validated);
    return derivation;
  }
  if (detached_wins) {
    derivation.state = PortAttachmentState::Empty;
    derivation.authority = detached.authority;
    derivation.validation = detached.validation;
    return derivation;
  }

  derivation.authority = attached.authority;
  derivation.validation = attached.validation;
  derivation.top = attached.top;
  for (const auto& entry : found->second) {
    if (!subject_record_live(state, entry.first, entry.second, policy)) {
      continue;
    }
    if (entry.second.value.object_incarnation != current || !entry.second.value.attached) {
      continue;
    }
    if (!(entry.second.meta.authority == attached.authority)) {
      continue;
    }
    derivation.ports.push_back(entry.second.value.port);
    derivation.slots.push_back(entry.second.value.slot);
  }
  std::sort(derivation.ports.begin(), derivation.ports.end());
  derivation.ports.erase(std::unique(derivation.ports.begin(), derivation.ports.end()), derivation.ports.end());
  derivation.state = derivation.ports.size() > 1 ? PortAttachmentState::Conflicting : PortAttachmentState::Attached;
  return derivation;
}

bool subject_double_booked(const RegistryState& state, const ObjectSideRef& subject, ValidationPolicy policy) {
  const SubjectDerivation derivation = derive_subject(state, subject, policy);
  return derivation.state == PortAttachmentState::Conflicting && derivation.ports.size() > 1;
}

bool connector_compatible(const ObjectSideDescriptor& side, const PortCapability& capability) noexcept {
  if (side.connector == ConnectorClass::Unknown || capability.accepted_connector == ConnectorClass::Unknown) {
    return true;
  }
  return side.connector == capability.accepted_connector;
}

bool media_compatible(const ObjectSideDescriptor& side, const PortCapability& capability) noexcept {
  if (side.media == MediaClass::Unknown || capability.accepted_media == MediaClass::Unknown) {
    return true;
  }
  return side.media == capability.accepted_media;
}

LifecycleAction lifecycle_action_of(const RegistryState& state,
                                    const ObjectId& object,
                                    bool& conflicting,
                                    std::vector<Attestation>& citations) {
  conflicting = false;
  citations.clear();

  struct Candidate {
    LifecycleAction action = LifecycleAction::Present;
    Authority authority{};
    bool from_reincarnation = false;
    Attestation meta;
  };
  std::vector<Candidate> candidates;

  const ObjectIncarnation current = detail::object_incarnation_of(state, object);
  const auto lifecycle = state.lifecycle_records.find(object);
  if (lifecycle != state.lifecycle_records.end()) {
    for (const auto& entry : lifecycle->second) {
      if (detail::session_is_fenced(state, entry.first)) {
        continue;
      }
      if (entry.second.value.object_incarnation != current) {
        continue;
      }
      candidates.push_back(Candidate{entry.second.value.action, entry.second.meta.authority, false,
                                     entry.second.meta});
    }
  }
  const auto reincarnation = state.reincarnation_records.find(object);
  if (reincarnation != state.reincarnation_records.end()) {
    for (const auto& entry : reincarnation->second) {
      if (detail::session_is_fenced(state, entry.first)) {
        continue;
      }
      if (entry.second.value.target != current) {
        continue;
      }
      candidates.push_back(
          Candidate{LifecycleAction::Present, entry.second.meta.authority, true, entry.second.meta});
    }
  }
  if (candidates.empty()) {
    return LifecycleAction::Present;
  }

  Authority best = candidates.front().authority;
  for (const Candidate& candidate : candidates) {
    if (best < candidate.authority) {
      best = candidate.authority;
    }
  }

  bool saw_present = false;
  bool saw_retired = false;
  bool saw_removed = false;
  bool saw_quarantined = false;
  bool present_from_reincarnation = false;
  for (const Candidate& candidate : candidates) {
    if (!(candidate.authority == best)) {
      continue;
    }
    citations.push_back(candidate.meta);
    switch (candidate.action) {
      case LifecycleAction::Present:
        saw_present = true;
        present_from_reincarnation = present_from_reincarnation || candidate.from_reincarnation;
        break;
      case LifecycleAction::Retired:
        saw_retired = true;
        break;
      case LifecycleAction::Removed:
        saw_removed = true;
        break;
      case LifecycleAction::Quarantined:
        saw_quarantined = true;
        break;
    }
  }

  const int distinct = (saw_present ? 1 : 0) + (saw_retired ? 1 : 0) + (saw_removed ? 1 : 0) +
                       (saw_quarantined ? 1 : 0);
  if (distinct <= 1) {
    if (saw_retired) {
      return LifecycleAction::Retired;
    }
    if (saw_removed) {
      return LifecycleAction::Removed;
    }
    if (saw_quarantined) {
      return LifecycleAction::Quarantined;
    }
    return LifecycleAction::Present;
  }
  // Retirement is sticky: another source at the same authority cannot bring a
  // retired identity back by declaring it present. Only a reincarnation at the
  // same authority, or a strictly higher authority, restores it. Within one
  // source stream the newest generation always wins, so the source that
  // retired the object can also correct itself.
  if (saw_retired && saw_present && present_from_reincarnation && distinct == 2) {
    return LifecycleAction::Present;
  }
  if (saw_retired) {
    return LifecycleAction::Retired;
  }
  if (saw_removed && saw_present && !saw_quarantined && distinct == 2) {
    return LifecycleAction::Removed;
  }
  conflicting = true;
  return LifecycleAction::Present;
}

struct LifecycleDerivation {
  LifecycleState state = LifecycleState::Registered;
  LifecycleAction action = LifecycleAction::Present;
  bool conflicting = false;
  std::vector<Attestation> citations;
};

LifecycleDerivation derive_lifecycle(const RegistryState& state,
                                     const ObjectId& object,
                                     bool has_attachment,
                                     bool has_detachment) {
  LifecycleDerivation derivation;
  derivation.action = lifecycle_action_of(state, object, derivation.conflicting, derivation.citations);
  if (derivation.conflicting) {
    derivation.state = LifecycleState::Conflicting;
    return derivation;
  }
  switch (derivation.action) {
    case LifecycleAction::Retired:
      derivation.state = LifecycleState::Retired;
      return derivation;
    case LifecycleAction::Removed:
      derivation.state = LifecycleState::Removed;
      return derivation;
    case LifecycleAction::Quarantined:
      derivation.state = LifecycleState::Quarantined;
      return derivation;
    case LifecycleAction::Present:
      break;
  }
  if (has_attachment) {
    derivation.state = LifecycleState::Attached;
  } else if (has_detachment) {
    derivation.state = LifecycleState::Unattached;
  } else {
    derivation.state = LifecycleState::Registered;
  }
  return derivation;
}

/// True when the object is excluded from the authoritative graph.
bool object_excluded(const LifecycleDerivation& lifecycle) noexcept {
  return lifecycle.state == LifecycleState::Quarantined || lifecycle.state == LifecycleState::Removed ||
         lifecycle.state == LifecycleState::Retired;
}

} // namespace

// ---------------------------------------------------------------------------
// Staged change
// ---------------------------------------------------------------------------

struct PortUpdate {
  PortSlotKey key;
  PortValue value;
};

/// One staged mutation. decide() fills it without touching the state, the
/// record is persisted, and apply() then writes it. A persistence failure
/// therefore leaves no trace in memory.
struct StagedChange {
  Attestation meta;

  bool has_object_registration = false;
  ObjectId object{};
  ObjectRegistrationValue object_registration;

  bool has_endpoint_registration = false;
  EndpointId endpoint{};
  EndpointRegistrationValue endpoint_registration;

  bool has_subject = false;
  ObjectSideRef subject{};
  SubjectValue subject_value;

  std::vector<PortUpdate> port_updates;

  bool has_lifecycle = false;
  ObjectId lifecycle_object{};
  LifecycleValue lifecycle;

  bool has_reincarnation = false;
  ObjectId reincarnation_object{};
  ReincarnationValue reincarnation;

  bool has_object_capability = false;
  ObjectId capability_object{};
  ObjectCapabilityValue object_capability;

  bool has_port_capability = false;
  PortRef capability_port{};
  PortCapabilityValue port_capability;

  /// True when the change must be written. A refused or duplicate record
  /// stages nothing.
  bool apply = false;

  /// A superseded record still belongs in the bounded supersession ring of the
  /// key it lost, so that the ring is a function of the record set rather than
  /// of the order the records arrived in. These flags stage that update only:
  /// no winning value is replaced.
  bool ring_only = false;
  bool ring_object_registration = false;
  bool ring_endpoint_registration = false;
  bool ring_subject = false;
  bool ring_lifecycle = false;
  bool ring_reincarnation = false;
  bool ring_object_capability = false;
  bool ring_port_capability = false;
  std::vector<PortSlotKey> ring_port_keys;
};

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Registry::Impl {
  RegistryOptions options;
  Limits limits;
  std::uint32_t history_per_key = 8;
  RegistryState state;
  std::unique_ptr<Store> store;
  RecoveryReport recovery;
  std::deque<RefusalView> refusals;
  mutable std::shared_mutex mutex;
  bool closed = false;

  /// Physical label to the objects that have registered it. The label reuse
  /// rule needs this lookup; scanning every object on every registration would
  /// make registering n objects quadratic in n. The index is a hint, not an
  /// authority: a hit is always re-checked against the object's own
  /// registrations, so a stale entry can only cost a look, never a wrong answer.
  mutable std::map<std::string, std::set<ObjectId>> label_index;
  mutable bool label_index_valid = false;

  void rebuild_label_index() const {
    label_index.clear();
    for (const auto& entry : state.object_registrations) {
      for (const auto& owner : entry.second) {
        const std::string& label = owner.second.value.descriptor.physical_label;
        if (!label.empty()) {
          label_index[label].insert(entry.first);
        }
      }
    }
    label_index_valid = true;
  }

  [[nodiscard]] const std::set<ObjectId>& label_holders(const std::string& label) const {
    if (!label_index_valid) {
      rebuild_label_index();
    }
    static const std::set<ObjectId> empty;
    const auto found = label_index.find(label);
    return found == label_index.end() ? empty : found->second;
  }

  /// Folds one attestation into a claim key, keeping the highest generations
  /// in the bounded supersession ring so the ring is a function of the record
  /// set rather than of its arrival order.
  template <class Key, class T>
  void fold(std::map<Key, Folded<T>>& slots, const Key& key, const Attestation& meta, const T& value) {
    auto found = slots.find(key);
    if (found == slots.end()) {
      Folded<T> folded;
      folded.meta = meta;
      folded.value = value;
      slots.emplace(key, std::move(folded));
      return;
    }
    Folded<T>& folded = found->second;
    bool already_present = false;
    for (const Attestation& existing : folded.superseded) {
      if (existing.evidence == folded.meta.evidence) {
        already_present = true;
        break;
      }
    }
    if (!already_present) {
      folded.superseded.push_back(folded.meta);
    }
    std::sort(folded.superseded.begin(),
              folded.superseded.end(),
              [](const Attestation& left, const Attestation& right) {
                if (left.generation != right.generation) {
                  return left.generation < right.generation;
                }
                return left.evidence < right.evidence;
              });
    while (folded.superseded.size() > history_per_key) {
      folded.superseded.erase(folded.superseded.begin());
      ++folded.history_dropped;
    }
    folded.meta = meta;
    folded.value = value;
  }

  /// Records a losing attestation in the same ring without changing the winner.
  /// An attestation that is already the winner, or already in the ring, is
  /// ignored, so replaying a record does not grow the ring.
  template <class Key, class T>
  void fold_losing(std::map<Key, Folded<T>>& slots, const Key& key, const Attestation& meta) {
    auto found = slots.find(key);
    if (found == slots.end()) {
      return;
    }
    Folded<T>& folded = found->second;
    if (folded.meta.evidence == meta.evidence) {
      return;
    }
    for (const Attestation& existing : folded.superseded) {
      if (existing.evidence == meta.evidence) {
        return;
      }
    }
    folded.superseded.push_back(meta);
    std::sort(folded.superseded.begin(),
              folded.superseded.end(),
              [](const Attestation& left, const Attestation& right) {
                if (left.generation != right.generation) {
                  return left.generation < right.generation;
                }
                return left.evidence < right.evidence;
              });
    while (folded.superseded.size() > history_per_key) {
      folded.superseded.erase(folded.superseded.begin());
      ++folded.history_dropped;
    }
  }

  void apply_ring_only(const StagedChange& change) {
    if (change.ring_object_registration) {
      auto group = state.object_registrations.find(change.object);
      if (group != state.object_registrations.end()) {
        fold_losing(group->second, change.meta.owner, change.meta);
      }
    }
    if (change.ring_endpoint_registration) {
      auto group = state.endpoint_registrations.find(change.endpoint);
      if (group != state.endpoint_registrations.end()) {
        fold_losing(group->second, change.meta.owner, change.meta);
      }
    }
    if (change.ring_subject) {
      auto group = state.subject_records.find(change.subject);
      if (group != state.subject_records.end()) {
        fold_losing(group->second, change.meta.owner, change.meta);
      }
    }
    if (change.ring_lifecycle) {
      auto group = state.lifecycle_records.find(change.lifecycle_object);
      if (group != state.lifecycle_records.end()) {
        fold_losing(group->second, change.meta.owner, change.meta);
      }
    }
    if (change.ring_reincarnation) {
      auto group = state.reincarnation_records.find(change.reincarnation_object);
      if (group != state.reincarnation_records.end()) {
        fold_losing(group->second, change.meta.owner, change.meta);
      }
    }
    if (change.ring_object_capability) {
      auto group = state.object_capability_records.find(change.capability_object);
      if (group != state.object_capability_records.end()) {
        fold_losing(group->second, change.meta.owner, change.meta);
      }
    }
    if (change.ring_port_capability) {
      auto group = state.port_capability_records.find(change.capability_port);
      if (group != state.port_capability_records.end()) {
        fold_losing(group->second, change.meta.owner, change.meta);
      }
    }
    for (const PortSlotKey& key : change.ring_port_keys) {
      auto group = state.port_records.find(key.port);
      if (group != state.port_records.end()) {
        fold_losing(group->second, key, change.meta);
      }
    }
  }

  void apply_change(const StagedChange& change) {
    apply_ring_only(change);
    if (change.ring_only) {
      return;
    }
    if (change.has_object_registration) {
      fold(state.object_registrations[change.object], change.meta.owner, change.meta,
           change.object_registration);
      if (label_index_valid) {
        const std::string& label = change.object_registration.descriptor.physical_label;
        if (!label.empty()) {
          label_index[label].insert(change.object);
        }
      }
    }
    if (change.has_endpoint_registration) {
      fold(state.endpoint_registrations[change.endpoint], change.meta.owner, change.meta,
           change.endpoint_registration);
    }
    if (change.has_subject) {
      fold(state.subject_records[change.subject], change.meta.owner, change.meta, change.subject_value);
    }
    for (const PortUpdate& update : change.port_updates) {
      fold(state.port_records[update.key.port], update.key, change.meta, update.value);
    }
    if (change.has_lifecycle) {
      fold(state.lifecycle_records[change.lifecycle_object], change.meta.owner, change.meta, change.lifecycle);
    }
    if (change.has_reincarnation) {
      fold(state.reincarnation_records[change.reincarnation_object], change.meta.owner, change.meta,
           change.reincarnation);
    }
    if (change.has_object_capability) {
      fold(state.object_capability_records[change.capability_object], change.meta.owner, change.meta,
           change.object_capability);
    }
    if (change.has_port_capability) {
      fold(state.port_capability_records[change.capability_port], change.meta.owner, change.meta,
           change.port_capability);
    }
  }

  IngestResult ingest_locked(const Evidence& evidence, Timestamp received_at, bool persist, bool live);
  bool persist_evidence(const Evidence& evidence, Timestamp received_at);
  void decide(const Evidence& evidence, const Attestation& meta, StagedChange& change, IngestResult& result) const;

  void note_refusal(const IngestResult& result) {
    RefusalView view;
    view.evidence = result.evidence;
    view.kind = result.kind;
    view.disposition = result.disposition;
    view.detail = result.detail;
    refusals.push_back(std::move(view));
    while (refusals.size() > limits.max_refusal_audit) {
      refusals.pop_front();
    }
  }
};

namespace {

/// Appends a rejection to the bounded refusal audit.
IngestDisposition effective_disposition(IngestDisposition base,
                                       bool session_fenced,
                                       bool fenced_incarnation,
                                       bool future_incarnation) noexcept {
  if (base == IngestDisposition::Duplicate || base == IngestDisposition::RefusedConflict ||
      base == IngestDisposition::AcceptedSuperseded || base == IngestDisposition::RefusedLabelConflict ||
      base == IngestDisposition::RefusedCapacity || base == IngestDisposition::RefusedInvalid) {
    return base;
  }
  if (session_fenced) {
    return IngestDisposition::AcceptedFencedSource;
  }
  if (fenced_incarnation) {
    return IngestDisposition::AcceptedFencedIncarnation;
  }
  if (future_incarnation) {
    return IngestDisposition::AcceptedPendingIncarnation;
  }
  return base;
}

/// True when the object is quarantined, removed or retired, and therefore out
/// of the authoritative graph. Only the lifecycle action is consulted, so this
/// never recurses into attachment derivation.
bool object_excluded(const RegistryState& state, const ObjectId& object) {
  bool conflicting = false;
  std::vector<Attestation> citations;
  const LifecycleAction action = lifecycle_action_of(state, object, conflicting, citations);
  return action == LifecycleAction::Quarantined || action == LifecycleAction::Removed ||
         action == LifecycleAction::Retired;
}

} // namespace

bool Registry::Impl::persist_evidence(const Evidence& evidence, Timestamp received_at) {
  if (!store) {
    return true;
  }
  Encoder encoder;
  encoder.u32(1);
  encoder.i64(received_at.unix_nanos);
  encode_evidence(encoder, evidence, limits);
  if (!encoder.ok()) {
    return false;
  }
  return store->Append(RecordKind::Evidence, encoder.view()).has_value();
}

namespace {

/// How an incoming record relates to whatever already occupies a claim key.
struct KeyPlan {
  bool write = false;
  bool ring = false;
  bool duplicate = false;
  bool collision = false;

  [[nodiscard]] bool any_write_or_ring() const noexcept { return write || ring; }
};

KeyPlan plan_of(GenerationOutcome outcome) {
  KeyPlan plan;
  switch (outcome) {
    case GenerationOutcome::Insert:
      plan.write = true;
      break;
    case GenerationOutcome::Superseded:
      plan.ring = true;
      break;
    case GenerationOutcome::Duplicate:
      plan.duplicate = true;
      break;
    case GenerationOutcome::Collision:
      plan.collision = true;
      break;
  }
  return plan;
}

/// The outcome of writing one claim key, given what already lives there.
template <class PrimaryKey, class OwnerKey, class Value>
GenerationOutcome existing_outcome(const std::map<PrimaryKey, std::map<OwnerKey, Folded<Value>>>& groups,
                                   const PrimaryKey& key,
                                   const OwnerKey& owner,
                                   const Attestation& meta,
                                   const Value& value) {
  const auto group = groups.find(key);
  if (group == groups.end()) {
    return GenerationOutcome::Insert;
  }
  const auto existing = group->second.find(owner);
  if (existing == group->second.end()) {
    return GenerationOutcome::Insert;
  }
  return compare_generation(existing->second.meta, meta, existing->second.value == value);
}

template <class T, class Key>
GenerationOutcome existing_outcome(const std::map<T, std::map<Key, Folded<typename T::mapped_type::mapped_type>>>&,
                                   ...) = delete;

} // namespace

void Registry::Impl::decide(const Evidence& evidence,
                            const Attestation& meta,
                            StagedChange& change,
                            IngestResult& result) const {
  const bool session_fenced = detail::session_is_fenced(state, meta.owner);
  change.meta = meta;
  result.disposition = IngestDisposition::Accepted;

  auto finish = [&](IngestDisposition base, bool fenced_incarnation, bool future_incarnation) {
    result.disposition = effective_disposition(base, session_fenced, fenced_incarnation, future_incarnation);
  };

  auto new_port_key = [&](const PortRef& port, const AttachmentSlot& slot) {
    PortSlotKey key;
    key.owner = meta.owner;
    key.port = port;
    key.slot = slot;
    return key;
  };

  auto port_capacity_reached = [&](const PortRef& port, const PortSlotKey& key) {
    const auto found = state.port_records.find(port);
    if (found == state.port_records.end()) {
      return false;
    }
    if (found->second.find(key) != found->second.end()) {
      return false;
    }
    return found->second.size() >= limits.max_claim_records_per_port;
  };

  // The record kinds that touch several claim keys resolve each key on its own:
  // a key that is new is written, a key that lost to a newer generation only
  // feeds the supersession ring, and a key that already holds the identical
  // record is left alone. Collapsing those into one answer per record would
  // make the stored set depend on the order the records arrived in.

  switch (kind_of(evidence)) {
    case EvidenceKind::RegisterObject: {
      const ObjectDescriptor& descriptor = std::get<RegisterObjectPayload>(evidence.payload).descriptor;
      change.object = descriptor.id;
      change.object_registration.descriptor = descriptor;

      const auto group = state.object_registrations.find(descriptor.id);
      GenerationOutcome outcome = GenerationOutcome::Insert;
      if (group != state.object_registrations.end()) {
        const auto existing = group->second.find(meta.owner);
        if (existing != group->second.end()) {
          outcome = compare_generation(existing->second.meta, meta,
                                       existing->second.value.descriptor == descriptor);
        }
      }
      if (outcome == GenerationOutcome::Collision) {
        result.disposition = IngestDisposition::RefusedConflict;
        result.detail = "generation " + std::to_string(meta.generation.value) +
                        " already carries a different object registration";
        return;
      }
      if (outcome == GenerationOutcome::Duplicate) {
        result.disposition = IngestDisposition::Duplicate;
        result.detail = "the object registration is already recorded";
        return;
      }
      if (outcome == GenerationOutcome::Superseded) {
        change.apply = true;
        change.ring_object_registration = true;
        result.disposition = IngestDisposition::AcceptedSuperseded;
        result.detail = "a newer generation of this source already registered the object";
        return;
      }

      if (!descriptor.physical_label.empty()) {
        for (const ObjectId& holder : label_holders(descriptor.physical_label)) {
          if (holder == descriptor.id) {
            continue;
          }
          const auto other = state.object_registrations.find(holder);
          if (other == state.object_registrations.end()) {
            continue;
          }
          bool holds_label = false;
          for (const auto& owner : other->second) {
            if (owner.second.value.descriptor.physical_label == descriptor.physical_label) {
              holds_label = true;
              break;
            }
          }
          if (!holds_label) {
            continue;
          }
          bool lifecycle_conflicting = false;
          std::vector<Attestation> citations;
          const LifecycleAction action = lifecycle_action_of(state, holder, lifecycle_conflicting, citations);
          if (action == LifecycleAction::Retired) {
            continue;
          }
          result.disposition = IngestDisposition::RefusedLabelConflict;
          result.detail = "the physical label is already bound to live object " + holder.to_hex();
          return;
        }
      }

      if (group == state.object_registrations.end() && state.object_registrations.size() >= limits.max_objects) {
        result.disposition = IngestDisposition::RefusedCapacity;
        result.detail = "the registry has reached its object limit";
        return;
      }
      change.has_object_registration = true;
      change.apply = true;
      finish(IngestDisposition::Accepted, false, false);
      return;
    }

    case EvidenceKind::RegisterEndpoint: {
      const EndpointDescriptor& descriptor = std::get<RegisterEndpointPayload>(evidence.payload).descriptor;
      change.endpoint = descriptor.id;
      change.endpoint_registration.descriptor = descriptor;

      const auto group = state.endpoint_registrations.find(descriptor.id);
      GenerationOutcome outcome = GenerationOutcome::Insert;
      if (group != state.endpoint_registrations.end()) {
        const auto existing = group->second.find(meta.owner);
        if (existing != group->second.end()) {
          outcome = compare_generation(existing->second.meta, meta,
                                       existing->second.value.descriptor == descriptor);
        }
      }
      if (outcome == GenerationOutcome::Collision) {
        result.disposition = IngestDisposition::RefusedConflict;
        result.detail = "generation " + std::to_string(meta.generation.value) +
                        " already carries a different endpoint registration";
        return;
      }
      if (outcome == GenerationOutcome::Duplicate) {
        result.disposition = IngestDisposition::Duplicate;
        result.detail = "the endpoint registration is already recorded";
        return;
      }
      if (outcome == GenerationOutcome::Superseded) {
        change.apply = true;
        change.ring_endpoint_registration = true;
        result.disposition = IngestDisposition::AcceptedSuperseded;
        result.detail = "a newer generation of this source already registered the endpoint";
        return;
      }
      if (group == state.endpoint_registrations.end() &&
          state.endpoint_registrations.size() >= limits.max_endpoints) {
        result.disposition = IngestDisposition::RefusedCapacity;
        result.detail = "the registry has reached its endpoint limit";
        return;
      }
      change.has_endpoint_registration = true;
      change.apply = true;
      finish(IngestDisposition::Accepted, false, false);
      return;
    }

    case EvidenceKind::Attach:
    case EvidenceKind::Detach:
    case EvidenceKind::Move: {
      // One record, several claim keys. Each key is resolved independently.
      ObjectSideRef subject{};
      ObjectIncarnation incarnation{};
      bool attached = false;
      bool via_move = false;
      PortRef move_from{};
      std::vector<PortUpdate> updates;

      if (kind_of(evidence) == EvidenceKind::Attach) {
        const auto& payload = std::get<AttachPayload>(evidence.payload);
        subject = payload.subject;
        incarnation = payload.object_incarnation;
        attached = true;
        PortUpdate update;
        update.key = new_port_key(payload.port, payload.slot);
        update.value = PortValue{payload.object_incarnation, false, payload.subject};
        updates.push_back(update);
      } else if (kind_of(evidence) == EvidenceKind::Detach) {
        const auto& payload = std::get<DetachPayload>(evidence.payload);
        subject = payload.subject;
        incarnation = payload.object_incarnation;
        attached = false;
        if (payload.has_from_port) {
          PortUpdate update;
          update.key = new_port_key(payload.from_port, payload.slot);
          update.value = PortValue{payload.object_incarnation, true, payload.subject};
          updates.push_back(update);
        }
      } else {
        const auto& payload = std::get<MovePayload>(evidence.payload);
        subject = payload.subject;
        incarnation = payload.object_incarnation;
        attached = true;
        via_move = true;
        move_from = payload.from;
        PortUpdate destination;
        destination.key = new_port_key(payload.to, payload.to_slot);
        destination.value = PortValue{payload.object_incarnation, false, payload.subject};
        updates.push_back(destination);
        const bool same_place = payload.from == payload.to && payload.from_slot.value == payload.to_slot.value;
        if (!same_place) {
          PortUpdate origin;
          origin.key = new_port_key(payload.from, payload.from_slot);
          origin.value = PortValue{payload.object_incarnation, true, payload.subject};
          updates.push_back(origin);
        }
      }

      change.subject = subject;
      change.subject_value = SubjectValue{incarnation, attached, {}, {}, via_move, move_from};
      if (attached) {
        change.subject_value.port = updates.front().value.empty ? PortRef{} : updates.front().key.port;
        change.subject_value.slot = updates.front().key.slot;
      }

      for (const PortUpdate& update : updates) {
        if (port_capacity_reached(update.key.port, update.key)) {
          result.disposition = IngestDisposition::RefusedCapacity;
          result.detail = "the port already carries the maximum number of distinct claims";
          return;
        }
      }

      const SubjectValue subject_value = change.subject_value;
      const GenerationOutcome subject_outcome =
          existing_outcome(state.subject_records, subject, meta.owner, meta, subject_value);

      std::vector<KeyPlan> port_outcomes;
      port_outcomes.reserve(updates.size());
      for (const PortUpdate& update : updates) {
        const auto group = state.port_records.find(update.key.port);
        if (group == state.port_records.end()) {
          port_outcomes.push_back(plan_of(GenerationOutcome::Insert));
          continue;
        }
        const auto existing = group->second.find(update.key);
        if (existing == group->second.end()) {
          port_outcomes.push_back(plan_of(GenerationOutcome::Insert));
          continue;
        }
        port_outcomes.push_back(
            plan_of(compare_generation(existing->second.meta, meta, existing->second.value == update.value)));
      }

      const KeyPlan subject_plan = plan_of(subject_outcome);
      bool collision = subject_plan.collision;
      for (const KeyPlan& plan : port_outcomes) {
        collision = collision || plan.collision;
      }
      if (collision) {
        result.disposition = IngestDisposition::RefusedConflict;
        result.detail = "generation " + std::to_string(meta.generation.value) +
                        " already carries different content for this claim key";
        return;
      }

      bool writes = subject_plan.write;
      bool rings = subject_plan.ring;
      for (const KeyPlan& plan : port_outcomes) {
        writes = writes || plan.write;
        rings = rings || plan.ring;
      }
      if (!writes && !rings) {
        result.disposition = IngestDisposition::Duplicate;
        result.detail = "the record is already stored at this generation";
        return;
      }

      if (subject_plan.write) {
        change.has_subject = true;
      }
      change.ring_subject = subject_plan.ring;
      for (std::size_t index = 0; index < updates.size(); ++index) {
        if (port_outcomes[index].write) {
          change.port_updates.push_back(updates[index]);
        }
        if (port_outcomes[index].ring) {
          change.ring_port_keys.push_back(updates[index].key);
        }
      }
      change.apply = true;

      if (!writes) {
        result.disposition = IngestDisposition::AcceptedSuperseded;
        result.detail = "a newer generation of this source already owns every claim key";
        return;
      }

      const ObjectIncarnation current = detail::object_incarnation_of(state, subject.object);
      bool pending = !detail::object_exists(state, subject.object);
      for (const PortUpdate& update : updates) {
        pending = pending || !detail::port_exists(state, update.key.port);
      }
      finish(pending ? IngestDisposition::AcceptedPendingReference : IngestDisposition::Accepted,
             incarnation < current,
             incarnation > current);
      return;
    }

    case EvidenceKind::SetLifecycle: {
      const auto& payload = std::get<SetLifecyclePayload>(evidence.payload);
      change.lifecycle_object = payload.object;
      change.lifecycle = LifecycleValue{payload.object_incarnation, payload.action, payload.reason};

      const auto group = state.lifecycle_records.find(payload.object);
      GenerationOutcome outcome = GenerationOutcome::Insert;
      if (group != state.lifecycle_records.end()) {
        const auto existing = group->second.find(meta.owner);
        if (existing != group->second.end()) {
          outcome = compare_generation(existing->second.meta, meta,
                                       existing->second.value.action == payload.action &&
                                           existing->second.value.reason == payload.reason);
        }
      }
      if (outcome == GenerationOutcome::Collision) {
        result.disposition = IngestDisposition::RefusedConflict;
        result.detail = "generation " + std::to_string(meta.generation.value) +
                        " already carries a different lifecycle assertion";
        return;
      }
      if (outcome == GenerationOutcome::Duplicate) {
        result.disposition = IngestDisposition::Duplicate;
        result.detail = "the lifecycle assertion is already recorded";
        return;
      }
      if (outcome == GenerationOutcome::Superseded) {
        change.apply = true;
        change.ring_lifecycle = true;
        result.disposition = IngestDisposition::AcceptedSuperseded;
        result.detail = "a newer generation of this source already asserted the lifecycle";
        return;
      }

      const ObjectIncarnation current = detail::object_incarnation_of(state, payload.object);
      change.has_lifecycle = true;
      change.apply = true;
      finish(detail::object_exists(state, payload.object) ? IngestDisposition::Accepted
                                                          : IngestDisposition::AcceptedPendingReference,
             payload.object_incarnation < current,
             payload.object_incarnation > current);
      return;
    }

    case EvidenceKind::ReincarnateObject: {
      const auto& payload = std::get<ReincarnateObjectPayload>(evidence.payload);
      change.reincarnation_object = payload.object;
      ReincarnationValue value;
      value.from = payload.from_incarnation;
      value.target = ObjectIncarnation{payload.from_incarnation.value + 1};
      value.serial_like = payload.new_serial_like;
      value.reason = payload.reason;
      change.reincarnation = value;

      const auto group = state.reincarnation_records.find(payload.object);
      GenerationOutcome outcome = GenerationOutcome::Insert;
      if (group != state.reincarnation_records.end()) {
        const auto existing = group->second.find(meta.owner);
        if (existing != group->second.end()) {
          outcome = compare_generation(existing->second.meta, meta,
                                       existing->second.value.from == value.from &&
                                           existing->second.value.serial_like == value.serial_like &&
                                           existing->second.value.reason == value.reason);
        }
      }
      if (outcome == GenerationOutcome::Collision) {
        result.disposition = IngestDisposition::RefusedConflict;
        result.detail = "generation " + std::to_string(meta.generation.value) +
                        " already carries a different reincarnation";
        return;
      }
      if (outcome == GenerationOutcome::Duplicate) {
        result.disposition = IngestDisposition::Duplicate;
        result.detail = "the reincarnation is already recorded";
        return;
      }
      if (outcome == GenerationOutcome::Superseded) {
        change.apply = true;
        change.ring_reincarnation = true;
        result.disposition = IngestDisposition::AcceptedSuperseded;
        result.detail = "a newer generation of this source already asserted a reincarnation";
        return;
      }
      if (!detail::object_exists(state, payload.object)) {
        change.has_reincarnation = true;
        change.apply = true;
        finish(IngestDisposition::AcceptedPendingReference, false, false);
        return;
      }
      const ObjectIncarnation current = detail::object_incarnation_of(state, payload.object);
      if (value.target <= current) {
        change.apply = true;
        change.ring_reincarnation = true;
        result.disposition = IngestDisposition::AcceptedSuperseded;
        result.detail = "the object is already at incarnation " + std::to_string(current.value) + " or beyond";
        return;
      }
      change.has_reincarnation = true;
      change.apply = true;
      finish(IngestDisposition::Accepted, false, false);
      return;
    }

    case EvidenceKind::PublishObjectCapability: {
      const auto& payload = std::get<PublishObjectCapabilityPayload>(evidence.payload);
      change.capability_object = payload.object;
      change.object_capability.capability = payload.capability;

      const auto group = state.object_capability_records.find(payload.object);
      GenerationOutcome outcome = GenerationOutcome::Insert;
      if (group != state.object_capability_records.end()) {
        const auto existing = group->second.find(meta.owner);
        if (existing != group->second.end()) {
          outcome = compare_generation(existing->second.meta, meta,
                                       existing->second.value.capability == payload.capability);
        }
      }
      if (outcome == GenerationOutcome::Collision) {
        result.disposition = IngestDisposition::RefusedConflict;
        result.detail = "generation " + std::to_string(meta.generation.value) +
                        " already carries a different capability";
        return;
      }
      if (outcome == GenerationOutcome::Duplicate) {
        result.disposition = IngestDisposition::Duplicate;
        result.detail = "the capability publication is already recorded";
        return;
      }
      if (outcome == GenerationOutcome::Superseded) {
        change.apply = true;
        change.ring_object_capability = true;
        result.disposition = IngestDisposition::AcceptedSuperseded;
        result.detail = "a newer generation of this source already published the capability";
        return;
      }
      change.has_object_capability = true;
      change.apply = true;
      finish(detail::object_exists(state, payload.object) ? IngestDisposition::Accepted
                                                          : IngestDisposition::AcceptedPendingReference,
             false,
             false);
      return;
    }

    case EvidenceKind::PublishPortCapability: {
      const auto& payload = std::get<PublishPortCapabilityPayload>(evidence.payload);
      change.capability_port = payload.port;
      change.port_capability.capability = payload.capability;

      const auto group = state.port_capability_records.find(payload.port);
      GenerationOutcome outcome = GenerationOutcome::Insert;
      if (group != state.port_capability_records.end()) {
        const auto existing = group->second.find(meta.owner);
        if (existing != group->second.end()) {
          outcome = compare_generation(existing->second.meta, meta,
                                       existing->second.value.capability == payload.capability);
        }
      }
      if (outcome == GenerationOutcome::Collision) {
        result.disposition = IngestDisposition::RefusedConflict;
        result.detail = "generation " + std::to_string(meta.generation.value) +
                        " already carries a different port capability";
        return;
      }
      if (outcome == GenerationOutcome::Duplicate) {
        result.disposition = IngestDisposition::Duplicate;
        result.detail = "the port capability publication is already recorded";
        return;
      }
      if (outcome == GenerationOutcome::Superseded) {
        change.apply = true;
        change.ring_port_capability = true;
        result.disposition = IngestDisposition::AcceptedSuperseded;
        result.detail = "a newer generation of this source already published the port capability";
        return;
      }
      change.has_port_capability = true;
      change.apply = true;
      finish(detail::port_exists(state, payload.port) ? IngestDisposition::Accepted
                                                      : IngestDisposition::AcceptedPendingReference,
             false,
             false);
      return;
    }
  }
  result.disposition = IngestDisposition::RefusedInvalid;
  result.detail = "the evidence kind is not recognised";
}

IngestResult Registry::Impl::ingest_locked(const Evidence& evidence,
                                           Timestamp received_at,
                                           bool persist,
                                           bool live) {
  IngestResult result;
  result.evidence = evidence.header.id;
  result.kind = kind_of(evidence);
  result.received_at = received_at;

  if (closed) {
    result.disposition = IngestDisposition::RefusedClosed;
    result.detail = "the registry is stopping and no longer accepts work";
    return result;
  }

  std::string error;
  if (!validate_evidence_shape(evidence, limits, error)) {
    result.disposition = IngestDisposition::RefusedInvalid;
    result.detail = std::move(error);
    if (persist) {
      note_refusal(result);
    }
    return result;
  }

  const auto source = state.sources.find(evidence.header.source);
  if (source == state.sources.end() || !source->second.described) {
    result.disposition = IngestDisposition::RefusedUnknownSource;
    result.detail = "the source is not registered; open a session before publishing";
    if (persist) {
      note_refusal(result);
    }
    return result;
  }
  const SourceIncarnationKey key{evidence.header.source, evidence.header.incarnation};
  if (state.sessions.find(key) == state.sessions.end()) {
    result.disposition = IngestDisposition::RefusedUnknownSource;
    result.detail = "no session is open for source incarnation " +
                    std::to_string(evidence.header.incarnation.value);
    if (persist) {
      note_refusal(result);
    }
    return result;
  }

  Attestation meta;
  meta.owner = key;
  meta.generation = evidence.header.generation;
  meta.evidence = evidence.header.id;
  meta.observed_at = evidence.header.observed_at;
  meta.received_at = received_at;
  meta.provenance = evidence.header.provenance;
  meta.authority = source->second.descriptor.authority;
  meta.object_incarnation = object_incarnation_of_evidence(evidence);
  meta.asserted_in_lifetime = live;

  StagedChange change;
  decide(evidence, meta, change, result);

  if (change.apply && is_accepted(result.disposition)) {
    if (persist && !persist_evidence(evidence, received_at)) {
      result.disposition = IngestDisposition::RefusedPersistence;
      result.detail = "the record could not be made durable";
      change.apply = false;
    }
    if (change.apply) {
      apply_change(change);
    }
  }

  SessionRecord& session = state.sessions[key];
  if (evidence.header.generation > session.high_water) {
    if (session.high_water.value != 0) {
      session.missing_generations += evidence.header.generation.value - session.high_water.value - 1;
    }
    session.high_water = evidence.header.generation;
  }
  if (is_accepted(result.disposition)) {
    ++session.accepted;
    if (result.disposition == IngestDisposition::Duplicate) {
      ++session.duplicates;
    }
  } else {
    ++session.refused;
    if (persist) {
      note_refusal(result);
    }
  }
  result.high_water = session.high_water;
  return result;
}


namespace {

// ---------------------------------------------------------------------------
// Derivation
// ---------------------------------------------------------------------------

PortView derive_port_view(const RegistryState& state,
                          const Limits& limits,
                          const PortRef& port,
                          ValidationPolicy policy) {
  PortView view;
  view.port = port;
  view.capability = port_capability_of(state, port);

  const auto records = state.port_records.find(port);
  if (records == state.port_records.end() || records->second.empty()) {
    return view;
  }

  struct LiveRecord {
    SourceIncarnationKey owner;
    AttachmentSlot slot;
    const Folded<PortValue>* record;
  };
  std::vector<LiveRecord> live;
  for (const auto& entry : records->second) {
    const SourceIncarnationKey& owner = entry.first.owner;
    const Folded<PortValue>& folded = entry.second;
    if (detail::session_is_fenced(state, owner) || !participates(folded.meta, policy)) {
      continue;
    }
    const PortValue& value = folded.value;
    if (!detail::object_exists(state, value.subject.object) || object_excluded(state, value.subject.object)) {
      continue;
    }
    const ObjectIncarnation current = detail::object_incarnation_of(state, value.subject.object);
    if (!(value.object_incarnation == current)) {
      continue;
    }
    if (!value.empty) {
      // The port record is live only while the owning source still asserts
      // that this subject sits here. A later statement from the same stream
      // makes it stale, and a stale record contributes nothing.
      const auto subject_group = state.subject_records.find(value.subject);
      if (subject_group == state.subject_records.end()) {
        continue;
      }
      const auto owner_entry = subject_group->second.find(owner);
      if (owner_entry == subject_group->second.end()) {
        continue;
      }
      const SubjectValue& subject_value = owner_entry->second.value;
      if (!subject_value.attached || !(subject_value.port == port) || !(subject_value.slot == entry.first.slot) ||
          !(subject_value.object_incarnation == current) ||
          !(owner_entry->second.meta.generation == folded.meta.generation)) {
        continue;
      }
    }
    live.push_back(LiveRecord{owner, entry.first.slot, &folded});
  }
  if (live.empty()) {
    return view;
  }

  Authority best = live.front().record->meta.authority;
  for (const LiveRecord& record : live) {
    if (best < record.record->meta.authority) {
      best = record.record->meta.authority;
    }
  }

  std::vector<const LiveRecord*> occupied;
  std::vector<const LiveRecord*> empty;
  bool any_lifetime = false;
  bool all_lifetime = true;
  for (const LiveRecord& record : live) {
    if (!(record.record->meta.authority == best)) {
      continue;
    }
    any_lifetime = any_lifetime || record.record->meta.asserted_in_lifetime;
    all_lifetime = all_lifetime && record.record->meta.asserted_in_lifetime;
    if (record.record->value.empty) {
      empty.push_back(&record);
    } else {
      occupied.push_back(&record);
    }
  }
  view.validation = combine_validation(any_lifetime, all_lifetime);
  view.emptiness_claims_total = static_cast<std::uint32_t>(empty.size());
  for (const LiveRecord* record : empty) {
    view.emptiness_claims.push_back(to_provenance(record->record->meta));
  }
  std::sort(view.emptiness_claims.begin(), view.emptiness_claims.end(), claim_order);

  std::vector<ClaimProvenance> overridden;
  std::uint32_t overridden_total = 0;
  for (const LiveRecord& record : live) {
    if (record.record->meta.authority == best) {
      continue;
    }
    ++overridden_total;
    if (overridden.size() < limits.max_overridden_reported) {
      overridden.push_back(to_provenance(record.record->meta));
    }
  }
  std::sort(overridden.begin(), overridden.end(), claim_order);

  if (occupied.empty()) {
    view.state = empty.empty() ? PortAttachmentState::Unknown : PortAttachmentState::Empty;
    return view;
  }

  std::vector<ObjectSideRef> subjects;
  subjects.reserve(occupied.size());
  for (const LiveRecord* record : occupied) {
    subjects.push_back(record->record->value.subject);
  }
  std::sort(subjects.begin(), subjects.end());
  subjects.erase(std::unique(subjects.begin(), subjects.end()), subjects.end());

  bool double_booked = false;
  for (const ObjectSideRef& subject : subjects) {
    if (subject_double_booked(state, subject, policy)) {
      double_booked = true;
    }
  }
  const bool too_many = subjects.size() > view.capability.max_simultaneous_attachments;
  const bool mixed = !empty.empty();
  view.state = (too_many || mixed || double_booked) ? PortAttachmentState::Conflicting
                                                    : PortAttachmentState::Attached;

  for (const ObjectSideRef& subject : subjects) {
    AttachmentEdge edge;
    edge.subject = subject;
    edge.port = port;
    edge.authority = best;
    edge.conflicting = view.state == PortAttachmentState::Conflicting;
    edge.overridden = overridden;
    edge.overridden_total = overridden_total;
    bool edge_any = false;
    bool edge_all = true;
    for (const LiveRecord* record : occupied) {
      if (!(record->record->value.subject == subject)) {
        continue;
      }
      edge.claims.push_back(to_provenance(record->record->meta));
      edge.slot = record->slot;
      edge.object_incarnation = record->record->value.object_incarnation;
      edge_any = edge_any || record->record->meta.asserted_in_lifetime;
      edge_all = edge_all && record->record->meta.asserted_in_lifetime;
    }
    std::sort(edge.claims.begin(), edge.claims.end(), claim_order);
    edge.validation = combine_validation(edge_any, edge_all);
    const ObjectSideDescriptor descriptor = side_descriptor_of(state, subject.object, subject.side);
    edge.connector_compatible = connector_compatible(descriptor, view.capability);
    edge.media_compatible = media_compatible(descriptor, view.capability);
    if (!edge.connector_compatible) {
      ConflictNote note;
      note.reason = ConflictReason::IncompatibleConnector;
      note.subject = subject;
      note.port = port;
      note.detail = "the object side connector does not match the declared port connector";
      note.claims = edge.claims;
      view.conflicts.push_back(std::move(note));
    }
    if (!edge.media_compatible) {
      ConflictNote note;
      note.reason = ConflictReason::IncompatibleMedia;
      note.subject = subject;
      note.port = port;
      note.detail = "the object side media does not match the declared port media";
      note.claims = edge.claims;
      view.conflicts.push_back(std::move(note));
    }
    view.edges.push_back(std::move(edge));
  }

  if (too_many) {
    ConflictNote note;
    note.reason = ConflictReason::TooManyAttachments;
    note.port = port;
    note.detail = std::to_string(subjects.size()) + " distinct subjects claim a port that admits " +
                  std::to_string(view.capability.max_simultaneous_attachments);
    for (const LiveRecord* record : occupied) {
      note.claims.push_back(to_provenance(record->record->meta));
    }
    std::sort(note.claims.begin(), note.claims.end(), claim_order);
    view.conflicts.push_back(std::move(note));
  }
  if (mixed) {
    ConflictNote note;
    note.reason = ConflictReason::EmptyAndOccupied;
    note.port = port;
    note.detail = "equal authority asserts both that the port is empty and that it is occupied";
    for (const LiveRecord& record : live) {
      if (!(record.record->meta.authority == best)) {
        continue;
      }
      note.claims.push_back(to_provenance(record.record->meta));
    }
    std::sort(note.claims.begin(), note.claims.end(), claim_order);
    view.conflicts.push_back(std::move(note));
  }
  if (double_booked) {
    for (const ObjectSideRef& subject : subjects) {
      if (!subject_double_booked(state, subject, policy)) {
        continue;
      }
      ConflictNote note;
      note.reason = ConflictReason::ObjectSideDoubleBooked;
      note.subject = subject;
      note.port = port;
      note.detail = "the object side is claimed at more than one port at the same authority";
      const SubjectDerivation derivation = derive_subject(state, subject, policy);
      for (const Attestation& attestation : derivation.top) {
        note.claims.push_back(to_provenance(attestation));
      }
      std::sort(note.claims.begin(), note.claims.end(), claim_order);
      view.conflicts.push_back(std::move(note));
    }
  }
  std::sort(view.edges.begin(), view.edges.end(), [](const AttachmentEdge& left, const AttachmentEdge& right) {
    return left.subject < right.subject;
  });
  return view;
}

std::vector<LifecycleEvent> collect_history(const RegistryState& state,
                                            const ObjectId& object,
                                            const Limits& limits,
                                            std::uint32_t max_events,
                                            std::uint32_t& dropped) {
  std::vector<LifecycleEvent> events;
  dropped = 0;

  auto push = [&](LifecycleEventKind kind, const Attestation& meta, const std::string& detail,
                  const ObjectSideRef* subject, const PortRef* port, const PortRef* from_port,
                  bool has_from) {
    LifecycleEvent event;
    event.kind = kind;
    event.observed_at = meta.observed_at;
    event.received_at = meta.received_at;
    event.source = meta.owner.source;
    event.incarnation = meta.owner.incarnation;
    event.generation = meta.generation;
    event.evidence = meta.evidence;
    event.provenance = meta.provenance;
    event.object_incarnation = detail::object_incarnation_of(state, object);
    if (subject != nullptr) {
      event.side = subject->side;
    }
    if (port != nullptr) {
      event.port = *port;
    }
    if (from_port != nullptr) {
      event.from_port = *from_port;
    }
    event.has_from_port = has_from;
    event.detail = detail;
    events.push_back(std::move(event));
  };

  const auto registrations = state.object_registrations.find(object);
  if (registrations != state.object_registrations.end()) {
    for (const auto& entry : registrations->second) {
      push(LifecycleEventKind::Registered, entry.second.meta, "registered", nullptr, nullptr, nullptr, false);
      for (const Attestation& superseded : entry.second.superseded) {
        push(LifecycleEventKind::Registered, superseded, "re-registered", nullptr, nullptr, nullptr, false);
      }
    }
  }

  const ObjectIncarnation current = detail::object_incarnation_of(state, object);
  const Folded<ObjectRegistrationValue>* registration = winning_registration(state, object);
  std::uint32_t side_count = 0;
  if (registration != nullptr) {
    side_count = static_cast<std::uint32_t>(registration->value.descriptor.sides.size());
  }
  for (const auto& entry : state.subject_records) {
    if (!(entry.first.object == object)) {
      continue;
    }
    for (const auto& owner : entry.second) {
      if (detail::session_is_fenced(state, owner.first)) {
        continue;
      }
      if (!(owner.second.value.object_incarnation == current)) {
        continue;
      }
      if (owner.second.value.attached) {
        const LifecycleEventKind kind =
            owner.second.value.via_move ? LifecycleEventKind::Moved : LifecycleEventKind::Attached;
        push(kind,
             owner.second.meta,
             owner.second.value.via_move ? "moved" : "attached",
             &entry.first,
             &owner.second.value.port,
             &owner.second.value.move_from,
             owner.second.value.via_move);
        for (const Attestation& superseded : owner.second.superseded) {
          push(kind, superseded, "superseded", &entry.first, &owner.second.value.port,
               &owner.second.value.move_from, owner.second.value.via_move);
        }
      } else {
        push(LifecycleEventKind::Detached,
             owner.second.meta,
             "detached",
             &entry.first,
             nullptr,
             nullptr,
             false);
        for (const Attestation& superseded : owner.second.superseded) {
          push(LifecycleEventKind::Detached, superseded, "superseded", &entry.first, nullptr, nullptr, false);
        }
      }
    }
  }
  (void)side_count;

  const auto lifecycle = state.lifecycle_records.find(object);
  if (lifecycle != state.lifecycle_records.end()) {
    for (const auto& entry : lifecycle->second) {
      if (detail::session_is_fenced(state, entry.first)) {
        continue;
      }
      LifecycleEventKind kind = LifecycleEventKind::Released;
      switch (entry.second.value.action) {
        case LifecycleAction::Present:
          kind = LifecycleEventKind::Released;
          break;
        case LifecycleAction::Quarantined:
          kind = LifecycleEventKind::Quarantined;
          break;
        case LifecycleAction::Removed:
          kind = LifecycleEventKind::Removed;
          break;
        case LifecycleAction::Retired:
          kind = LifecycleEventKind::Retired;
          break;
      }
      push(kind, entry.second.meta, entry.second.value.reason, nullptr, nullptr, nullptr, false);
    }
  }

  const auto reincarnation = state.reincarnation_records.find(object);
  if (reincarnation != state.reincarnation_records.end()) {
    for (const auto& entry : reincarnation->second) {
      if (detail::session_is_fenced(state, entry.first)) {
        continue;
      }
      push(LifecycleEventKind::Reincarnated,
           entry.second.meta,
           entry.second.value.reason,
           nullptr,
           nullptr,
           nullptr,
           false);
    }
  }

  std::sort(events.begin(), events.end(), [](const LifecycleEvent& left, const LifecycleEvent& right) {
    if (!(left.observed_at == right.observed_at)) {
      return left.observed_at < right.observed_at;
    }
    if (left.source != right.source) {
      return left.source < right.source;
    }
    if (!(left.incarnation == right.incarnation)) {
      return left.incarnation < right.incarnation;
    }
    if (!(left.generation == right.generation)) {
      return left.generation < right.generation;
    }
    return left.evidence < right.evidence;
  });
  if (events.size() > max_events) {
    dropped = static_cast<std::uint32_t>(events.size() - max_events);
    events.erase(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(dropped));
  }
  (void)limits;
  return events;
}

ObjectView derive_object_view(const RegistryState& state,
                              const Limits& limits,
                              const ObjectId& object,
                              const QueryOptions& options,
                              std::uint32_t history_per_key) {
  (void)history_per_key;
  ObjectView view;
  const Folded<ObjectRegistrationValue>* registration = winning_registration(state, object);
  if (registration == nullptr) {
    return view;
  }
  view.id = object;
  view.kind = registration->value.descriptor.kind;
  view.physical_label = registration->value.descriptor.physical_label;
  view.serial_like = registration->value.descriptor.serial_like;
  view.administrative_location = registration->value.descriptor.administrative_location;
  view.capability = registration->value.descriptor.capability;
  view.incarnation = detail::object_incarnation_of(state, object);

  // A reincarnation at the current incarnation carries the replacement unit's
  // serial-like identity.
  const auto reincarnation = state.reincarnation_records.find(object);
  if (reincarnation != state.reincarnation_records.end()) {
    const Folded<ReincarnationValue>* chosen = nullptr;
    SourceIncarnationKey chosen_key{};
    for (const auto& entry : reincarnation->second) {
      if (detail::session_is_fenced(state, entry.first)) {
        continue;
      }
      if (!(entry.second.value.target == view.incarnation)) {
        continue;
      }
      if (chosen == nullptr || chosen->meta.authority < entry.second.meta.authority ||
          (chosen->meta.authority == entry.second.meta.authority && entry.first < chosen_key)) {
        chosen = &entry.second;
        chosen_key = entry.first;
      }
    }
    if (chosen != nullptr && !chosen->value.serial_like.empty()) {
      view.serial_like = chosen->value.serial_like;
    }
  }

  const auto capability = state.object_capability_records.find(object);
  if (capability != state.object_capability_records.end()) {
    const Folded<ObjectCapabilityValue>* chosen = nullptr;
    SourceIncarnationKey chosen_key{};
    bool differing = false;
    Authority best{};
    for (const auto& entry : capability->second) {
      if (detail::session_is_fenced(state, entry.first)) {
        continue;
      }
      if (chosen == nullptr || best < entry.second.meta.authority) {
        best = entry.second.meta.authority;
        chosen = &entry.second;
        chosen_key = entry.first;
        differing = false;
      } else if (entry.second.meta.authority == best) {
        if (!(entry.second.value.capability == chosen->value.capability)) {
          differing = true;
        }
        if (entry.first < chosen_key) {
          chosen = &entry.second;
          chosen_key = entry.first;
        }
      }
    }
    if (chosen != nullptr) {
      view.capability = chosen->value.capability;
      if (differing) {
        ConflictNote note;
        note.reason = ConflictReason::CapabilityConflict;
        note.detail = "equal authority published different nominal capabilities for this object";
        view.conflicts.push_back(std::move(note));
      }
    }
  }

  // Registration disagreements at equal authority are reported, never hidden.
  {
    Authority best{};
    bool have = false;
    bool differing = false;
    const auto all = state.object_registrations.find(object);
    if (all != state.object_registrations.end()) {
      for (const auto& entry : all->second) {
        if (!have || best < entry.second.meta.authority) {
          best = entry.second.meta.authority;
          have = true;
          differing = false;
        } else if (entry.second.meta.authority == best &&
                   !(entry.second.value.descriptor == registration->value.descriptor)) {
          differing = true;
        }
      }
    }
    if (differing) {
      ConflictNote note;
      note.reason = ConflictReason::CapabilityConflict;
      note.detail = "equal authority registered different descriptors for this object";
      view.conflicts.push_back(std::move(note));
    }
  }

  bool has_attachment = false;
  bool has_detachment = false;
  bool any_lifetime = false;
  bool all_lifetime = true;
  bool saw_side = false;

  if (options.include_object_sides) {
    const std::size_t side_count = registration->value.descriptor.sides.size();
    for (std::size_t index = 0; index < side_count; ++index) {
      ObjectSideRef reference;
      reference.object = object;
      reference.side = SideIndex{static_cast<std::uint32_t>(index)};
      ObjectSideView side;
      side.side = reference.side;
      side.descriptor = side_descriptor_of(state, object, reference.side);
      const SubjectDerivation derivation = derive_subject(state, reference, options.validation_policy);
      side.state = derivation.state;
      side.validation = derivation.validation;
      if (derivation.has_records) {
        saw_side = true;
        any_lifetime = any_lifetime || derivation.validation != ValidationState::Unvalidated;
        all_lifetime = all_lifetime && derivation.validation == ValidationState::Validated;
      }
      if (derivation.state == PortAttachmentState::Attached ||
          derivation.state == PortAttachmentState::Conflicting) {
        has_attachment = true;
      }
      if (derivation.state == PortAttachmentState::Empty) {
        has_detachment = true;
      }
      for (std::size_t port_index = 0; port_index < derivation.ports.size(); ++port_index) {
        AttachmentEdge edge;
        edge.subject = reference;
        edge.port = derivation.ports[port_index];
        edge.slot = port_index < derivation.slots.size() ? derivation.slots[port_index] : AttachmentSlot{};
        edge.authority = derivation.authority;
        edge.validation = derivation.validation;
        edge.object_incarnation = derivation.incarnation;
        edge.conflicting = derivation.state == PortAttachmentState::Conflicting;
        for (const Attestation& attestation : derivation.top) {
          edge.claims.push_back(to_provenance(attestation));
        }
        std::sort(edge.claims.begin(), edge.claims.end(), claim_order);
        const PortCapability capability_of_port = port_capability_of(state, edge.port);
        edge.connector_compatible = connector_compatible(side.descriptor, capability_of_port);
        edge.media_compatible = media_compatible(side.descriptor, capability_of_port);
        side.edges.push_back(std::move(edge));
      }
      if (derivation.state == PortAttachmentState::Conflicting && derivation.ports.size() > 1) {
        ConflictNote note;
        note.reason = ConflictReason::ObjectSideDoubleBooked;
        note.subject = reference;
        note.detail = "the object side is claimed at more than one port at the same authority";
        for (const Attestation& attestation : derivation.top) {
          note.claims.push_back(to_provenance(attestation));
        }
        std::sort(note.claims.begin(), note.claims.end(), claim_order);
        side.conflicts.push_back(std::move(note));
      }
      view.sides.push_back(std::move(side));
    }
  } else {
    const std::size_t side_count = registration->value.descriptor.sides.size();
    for (std::size_t index = 0; index < side_count; ++index) {
      ObjectSideRef reference;
      reference.object = object;
      reference.side = SideIndex{static_cast<std::uint32_t>(index)};
      const SubjectDerivation derivation = derive_subject(state, reference, options.validation_policy);
      if (derivation.state == PortAttachmentState::Attached ||
          derivation.state == PortAttachmentState::Conflicting) {
        has_attachment = true;
      }
      if (derivation.state == PortAttachmentState::Empty) {
        has_detachment = true;
      }
    }
  }
  (void)saw_side;

  const LifecycleDerivation lifecycle =
      derive_lifecycle(state, object, has_attachment, has_detachment);
  view.lifecycle = lifecycle.state;
  if (lifecycle.conflicting) {
    ConflictNote note;
    note.reason = ConflictReason::LifecycleConflict;
    note.detail = "equal authority asserts incompatible lifecycle states";
    for (const Attestation& attestation : lifecycle.citations) {
      note.claims.push_back(to_provenance(attestation));
    }
    std::sort(note.claims.begin(), note.claims.end(), claim_order);
    view.conflicts.push_back(std::move(note));
  }
  view.validation = combine_validation(any_lifetime, all_lifetime);

  if (options.include_history) {
    view.history = collect_history(state, object, limits, limits.max_history_events, view.history_dropped);
  }
  return view;
}

GraphSummary summarize(const std::vector<PortView>& ports,
                       const std::vector<ObjectView>& objects,
                       std::uint64_t fenced,
                       std::uint64_t pending) {
  GraphSummary summary;
  for (const PortView& port : ports) {
    switch (port.state) {
      case PortAttachmentState::Unknown:
        ++summary.ports_unknown;
        break;
      case PortAttachmentState::Empty:
        ++summary.ports_empty;
        break;
      case PortAttachmentState::Attached:
        ++summary.ports_attached;
        break;
      case PortAttachmentState::Conflicting:
        ++summary.ports_conflicting;
        break;
    }
    summary.conflicts += port.conflicts.size();
    for (const AttachmentEdge& edge : port.edges) {
      ++summary.edges;
      if (edge.validation != ValidationState::Validated) {
        ++summary.edges_unvalidated;
      }
    }
  }
  for (const ObjectView& object : objects) {
    ++summary.objects_total;
    switch (object.lifecycle) {
      case LifecycleState::Registered:
        ++summary.objects_registered;
        break;
      case LifecycleState::Unattached:
        ++summary.objects_unattached;
        break;
      case LifecycleState::Attached:
        ++summary.objects_attached;
        break;
      case LifecycleState::Quarantined:
        ++summary.objects_quarantined;
        break;
      case LifecycleState::Removed:
        ++summary.objects_removed;
        break;
      case LifecycleState::Retired:
        ++summary.objects_retired;
        break;
      case LifecycleState::Conflicting:
        ++summary.conflicts;
        break;
    }
    summary.conflicts += object.conflicts.size();
  }
  summary.claims_fenced = fenced;
  summary.claims_pending = pending;
  return summary;
}

std::uint64_t count_fenced_claims(const RegistryState& state) {
  std::uint64_t total = 0;

  // Capability publications are dynamic claims and are fenced with their
  // source incarnation. Registrations are catalogue facts and are not.
  auto count_by_session = [&total, &state](const auto& groups) {
    for (const auto& group : groups) {
      for (const auto& owner : group.second) {
        if (detail::session_is_fenced(state, owner.first)) {
          total += 1 + owner.second.superseded.size();
        }
      }
    }
  };
  count_by_session(state.object_capability_records);
  count_by_session(state.port_capability_records);

  for (const auto& entry : state.subject_records) {
    const ObjectIncarnation current = detail::object_incarnation_of(state, entry.first.object);
    for (const auto& owner : entry.second) {
      if (detail::session_is_fenced(state, owner.first) ||
          (detail::object_exists(state, entry.first.object) &&
           !(owner.second.value.object_incarnation == current))) {
        total += 1 + owner.second.superseded.size();
      }
    }
  }
  for (const auto& entry : state.port_records) {
    for (const auto& owner : entry.second) {
      const ObjectId object = owner.second.value.subject.object;
      const bool incarnation_fenced =
          detail::object_exists(state, object) &&
          !(owner.second.value.object_incarnation == detail::object_incarnation_of(state, object));
      if (detail::session_is_fenced(state, owner.first.owner) || incarnation_fenced) {
        total += 1 + owner.second.superseded.size();
      }
    }
  }
  for (const auto& entry : state.lifecycle_records) {
    const ObjectIncarnation current = detail::object_incarnation_of(state, entry.first);
    for (const auto& owner : entry.second) {
      if (detail::session_is_fenced(state, owner.first) ||
          (detail::object_exists(state, entry.first) &&
           !(owner.second.value.object_incarnation == current))) {
        total += 1 + owner.second.superseded.size();
      }
    }
  }
  return total;
}

/// Counts records whose reference does not exist yet.
std::uint64_t count_pending_claims(const RegistryState& state) {
  std::uint64_t total = 0;
  for (const auto& entry : state.subject_records) {
    if (!detail::object_exists(state, entry.first.object)) {
      total += entry.second.size();
      continue;
    }
    const ObjectIncarnation current = detail::object_incarnation_of(state, entry.first.object);
    for (const auto& owner : entry.second) {
      if (detail::session_is_fenced(state, owner.first)) {
        continue;
      }
      if (owner.second.value.object_incarnation > current) {
        ++total;
      }
    }
  }
  for (const auto& entry : state.port_records) {
    const bool port_missing = !detail::port_exists(state, entry.first);
    for (const auto& owner : entry.second) {
      if (port_missing) {
        ++total;
        continue;
      }
      const ObjectId object = owner.second.value.subject.object;
      if (!detail::object_exists(state, object)) {
        ++total;
        continue;
      }
      if (owner.second.value.object_incarnation > detail::object_incarnation_of(state, object)) {
        ++total;
      }
    }
  }
  for (const auto& entry : state.lifecycle_records) {
    if (!detail::object_exists(state, entry.first)) {
      total += entry.second.size();
    }
  }
  for (const auto& entry : state.reincarnation_records) {
    if (!detail::object_exists(state, entry.first)) {
      total += entry.second.size();
    }
  }
  for (const auto& entry : state.object_capability_records) {
    if (!detail::object_exists(state, entry.first)) {
      total += entry.second.size();
    }
  }
  for (const auto& entry : state.port_capability_records) {
    if (!detail::port_exists(state, entry.first)) {
      total += entry.second.size();
    }
  }
  return total;
}

} // namespace


namespace {

// ---------------------------------------------------------------------------
// Session and snapshot helpers
// ---------------------------------------------------------------------------

void apply_session_open(RegistryState& state,
                        const SourceDescriptor& descriptor,
                        Incarnation incarnation,
                        Timestamp opened_at) {
  SourceRecord& source = state.sources[descriptor.id];
  if (!source.described || incarnation > source.descriptor_incarnation) {
    source.descriptor = descriptor;
    source.descriptor_incarnation = incarnation;
    source.described = true;
  }
  if (incarnation > source.current_incarnation) {
    source.current_incarnation = incarnation;
  }
  SessionRecord& session = state.sessions[SourceIncarnationKey{descriptor.id, incarnation}];
  if (session.opened_at.unix_nanos == 0) {
    session.opened_at = opened_at;
  }
}

std::uint64_t count_claims_before(const RegistryState& state, const SourceId& source, Incarnation incarnation) {
  std::uint64_t total = 0;
  auto scan = [&](const auto& groups) {
    for (const auto& group : groups) {
      for (const auto& owner : group.second) {
        if (owner.first.source == source && owner.first.incarnation < incarnation) {
          total += 1 + owner.second.superseded.size();
        }
      }
    }
  };
  scan(state.subject_records);
  scan(state.lifecycle_records);
  scan(state.reincarnation_records);
  scan(state.object_capability_records);
  scan(state.port_capability_records);
  for (const auto& port : state.port_records) {
    for (const auto& owner : port.second) {
      if (owner.first.owner.source == source && owner.first.owner.incarnation < incarnation) {
        total += 1 + owner.second.superseded.size();
      }
    }
  }
  return total;
}

std::uint64_t count_session_claims(const RegistryState& state, const SourceIncarnationKey& key) {
  std::uint64_t total = 0;
  auto scan = [&](const auto& groups) {
    for (const auto& group : groups) {
      for (const auto& owner : group.second) {
        if (owner.first == key) {
          total += 1 + owner.second.superseded.size();
        }
      }
    }
  };
  scan(state.subject_records);
  scan(state.lifecycle_records);
  scan(state.reincarnation_records);
  scan(state.object_capability_records);
  scan(state.port_capability_records);
  for (const auto& port : state.port_records) {
    for (const auto& owner : port.second) {
      if (owner.first.owner == key) {
        total += 1 + owner.second.superseded.size();
      }
    }
  }
  return total;
}

/// Claims fenced by a replacement, grouped by object. One pass over the
/// records rather than one pass per object: the per-object form is quadratic in
/// the size of the graph and dominates snapshot construction on a large one.
std::map<ObjectId, std::uint64_t> fenced_claims_by_object(const RegistryState& state) {
  std::map<ObjectId, std::uint64_t> totals;
  auto add = [&totals](const ObjectId& object, std::uint64_t count) {
    if (count == 0) {
      return;
    }
    totals[object] += count;
  };

  for (const auto& entry : state.subject_records) {
    const ObjectIncarnation current = detail::object_incarnation_of(state, entry.first.object);
    std::uint64_t count = 0;
    for (const auto& owner : entry.second) {
      if (!(owner.second.value.object_incarnation == current)) {
        count += 1 + owner.second.superseded.size();
      }
    }
    add(entry.first.object, count);
  }
  for (const auto& entry : state.port_records) {
    for (const auto& owner : entry.second) {
      const ObjectId object = owner.second.value.subject.object;
      if (owner.second.value.object_incarnation == detail::object_incarnation_of(state, object)) {
        continue;
      }
      add(object, 1 + owner.second.superseded.size());
    }
  }
  for (const auto& entry : state.lifecycle_records) {
    const ObjectIncarnation current = detail::object_incarnation_of(state, entry.first);
    std::uint64_t count = 0;
    for (const auto& owner : entry.second) {
      if (!(owner.second.value.object_incarnation == current)) {
        count += 1 + owner.second.superseded.size();
      }
    }
    add(entry.first, count);
  }
  return totals;
}

SnapshotClaim to_snapshot_claim(const ClaimProvenance& claim) {
  SnapshotClaim out;
  out.source = claim.source;
  out.incarnation = claim.incarnation;
  out.generation = claim.generation;
  out.evidence = claim.evidence;
  out.observed_at = claim.observed_at;
  out.provenance = claim.provenance;
  out.authority = claim.authority;
  return out;
}

SnapshotAttachment to_snapshot_attachment(const AttachmentEdge& edge) {
  SnapshotAttachment out;
  out.subject = edge.subject;
  out.port = edge.port;
  out.slot = edge.slot;
  out.object_incarnation = edge.object_incarnation;
  out.authority = edge.authority;
  out.connector_compatible = edge.connector_compatible;
  out.media_compatible = edge.media_compatible;
  out.conflicting = edge.conflicting;
  out.claims.reserve(edge.claims.size());
  for (const ClaimProvenance& claim : edge.claims) {
    out.claims.push_back(to_snapshot_claim(claim));
  }
  return out;
}

SnapshotConflict to_snapshot_conflict(const ConflictNote& note) {
  SnapshotConflict out;
  out.reason = note.reason;
  out.subject = note.subject;
  out.port = note.port;
  out.claims.reserve(note.claims.size());
  for (const ClaimProvenance& claim : note.claims) {
    out.claims.push_back(to_snapshot_claim(claim));
  }
  return out;
}

TopologySnapshot build_snapshot(const RegistryState& state, const Limits& limits) {
  TopologySnapshot snapshot;
  snapshot.format_version = kSnapshotFormatVersion;
  snapshot.registry = state.id;

  QueryOptions options;
  options.include_object_sides = true;
  options.include_history = false;
  options.validation_policy = ValidationPolicy::IncludeAll;

  std::vector<ObjectView> objects;
  objects.reserve(state.object_registrations.size());
  for (const auto& entry : state.object_registrations) {
    objects.push_back(derive_object_view(state, limits, entry.first, options, 0));
  }
  std::vector<PortView> ports;
  ports.reserve(state.port_records.size());
  for (const auto& entry : state.port_records) {
    ports.push_back(derive_port_view(state, limits, entry.first, ValidationPolicy::IncludeAll));
  }

  for (const ObjectView& view : objects) {
    SnapshotObject object;
    object.id = view.id;
    object.kind = view.kind;
    object.physical_label = view.physical_label;
    object.serial_like = view.serial_like;
    object.administrative_location = view.administrative_location;
    object.incarnation = view.incarnation;
    object.lifecycle = view.lifecycle;
    object.capability = view.capability;
    object.sides.reserve(view.sides.size());
    for (const ObjectSideView& side : view.sides) {
      object.sides.push_back(side.descriptor);
      for (const AttachmentEdge& edge : side.edges) {
        object.attachments.push_back(to_snapshot_attachment(edge));
      }
    }
    std::sort(object.attachments.begin(),
              object.attachments.end(),
              [](const SnapshotAttachment& left, const SnapshotAttachment& right) {
                if (left.port != right.port) {
                  return left.port < right.port;
                }
                return left.subject < right.subject;
              });
    snapshot.objects.push_back(std::move(object));
  }

  for (const auto& entry : state.endpoint_registrations) {
    const Folded<EndpointRegistrationValue>* registration = winning_endpoint(state, entry.first);
    if (registration == nullptr) {
      continue;
    }
    SnapshotEndpoint endpoint;
    endpoint.id = entry.first;
    endpoint.kind = registration->value.descriptor.kind;
    endpoint.name = registration->value.descriptor.name;
    endpoint.administrative_location = registration->value.descriptor.administrative_location;
    endpoint.port_count = registration->value.descriptor.port_count;
    endpoint.default_port_capability = registration->value.descriptor.default_port_capability;
    snapshot.endpoints.push_back(std::move(endpoint));
  }

  for (const PortView& view : ports) {
    SnapshotPort port;
    port.port = view.port;
    port.state = view.state;
    port.capability = view.capability;
    for (const AttachmentEdge& edge : view.edges) {
      port.attachments.push_back(to_snapshot_attachment(edge));
    }
    for (const ConflictNote& note : view.conflicts) {
      port.conflicts.push_back(to_snapshot_conflict(note));
    }
    snapshot.ports.push_back(std::move(port));
  }

  for (const auto& entry : state.sessions) {
    if (!detail::session_is_fenced(state, entry.first)) {
      continue;
    }
    const auto source = state.sources.find(entry.first.source);
    SnapshotFencedSession fenced;
    fenced.source = entry.first.source;
    fenced.incarnation = entry.first.incarnation;
    fenced.fenced_by = source == state.sources.end() ? Incarnation{} : source->second.current_incarnation;
    fenced.claims = count_session_claims(state, entry.first);
    snapshot.fenced_sessions.push_back(fenced);
  }

  const std::map<ObjectId, std::uint64_t> fenced_by_object = fenced_claims_by_object(state);
  for (const auto& entry : fenced_by_object) {
    if (entry.second == 0) {
      continue;
    }
    SnapshotFencedObject object;
    object.object = entry.first;
    object.current_incarnation = detail::object_incarnation_of(state, entry.first);
    object.fenced_claims = entry.second;
    snapshot.fenced_objects.push_back(object);
  }

  snapshot.summary = summarize(ports, objects, count_fenced_claims(state), count_pending_claims(state));
  // The canonical image is a function of the accepted evidence alone. A
  // validation counter depends on this registry lifetime, so it is zeroed
  // here; the envelope carries the validation counts instead.
  snapshot.summary.edges_unvalidated = 0;

  Encoder encoder;
  detail::encode_provenance_image(encoder, state, limits);
  const std::span<const std::byte> image = encoder.view();
  snapshot.provenance_digest = sha256(image);
  return snapshot;
}

RegistryStats build_stats(const RegistryState& state,
                          const RecoveryReport& recovery,
                          const Store* store) {
  RegistryStats stats;
  stats.registry = state.id;
  stats.opened_at = state.opened_at;
  stats.objects = state.object_registrations.size();
  stats.endpoints = state.endpoint_registrations.size();
  for (const auto& entry : state.endpoint_registrations) {
    stats.ports += detail::endpoint_port_count(state, entry.first);
  }
  stats.sources = state.sources.size();
  stats.sessions = state.sessions.size();
  std::uint64_t superseded = 0;
  auto add_superseded = [&superseded](const auto& groups) {
    for (const auto& group : groups) {
      for (const auto& owner : group.second) {
        superseded += owner.second.superseded.size();
      }
    }
  };
  add_superseded(state.object_registrations);
  add_superseded(state.endpoint_registrations);
  add_superseded(state.subject_records);
  add_superseded(state.lifecycle_records);
  add_superseded(state.reincarnation_records);
  add_superseded(state.object_capability_records);
  add_superseded(state.port_capability_records);
  for (const auto& port : state.port_records) {
    for (const auto& owner : port.second) {
      superseded += owner.second.superseded.size();
    }
  }
  for (const auto& entry : state.sessions) {
    if (entry.second.live) {
      ++stats.live_sessions;
    }
    stats.evidence_accepted += entry.second.accepted;
    stats.evidence_refused += entry.second.refused;
    stats.evidence_duplicate += entry.second.duplicates;
  }
  stats.evidence_superseded = superseded;
  stats.evidence_fenced = count_fenced_claims(state);
  stats.evidence_pending = count_pending_claims(state);
  for (const auto& entry : state.subject_records) {
    stats.claim_records += entry.second.size();
  }
  for (const auto& entry : state.port_records) {
    stats.port_records += entry.second.size();
  }
  for (const auto& entry : state.lifecycle_records) {
    stats.lifecycle_records += entry.second.size();
  }
  for (const auto& entry : state.object_capability_records) {
    stats.capability_records += entry.second.size();
  }
  for (const auto& entry : state.port_capability_records) {
    stats.capability_records += entry.second.size();
  }
  if (store != nullptr) {
    stats.store_records = store->records_in_log();
    stats.store_bytes = store->bytes_on_disk();
    stats.store_rewrites = store->rewrite_count();
  }
  stats.recovery = recovery;
  return stats;
}

std::vector<PortView> collect_ports_by_state(const RegistryState& state,
                                             const Limits& limits,
                                             PortAttachmentState wanted,
                                             ValidationPolicy policy) {
  std::vector<PortView> result;
  for (const auto& entry : state.port_records) {
    PortView view = derive_port_view(state, limits, entry.first, policy);
    if (view.state == wanted) {
      result.push_back(std::move(view));
      if (result.size() >= limits.max_snapshot_entries) {
        break;
      }
    }
  }
  return result;
}

} // namespace

const char* to_string(OpenSessionResult::Disposition value) noexcept {
  switch (value) {
    case OpenSessionResult::Disposition::Opened:
      return "opened";
    case OpenSessionResult::Disposition::Resumed:
      return "resumed";
    case OpenSessionResult::Disposition::Reopened:
      return "reopened";
    case OpenSessionResult::Disposition::NewIncarnation:
      return "new-incarnation";
    case OpenSessionResult::Disposition::RefusedFenced:
      return "refused-fenced";
    case OpenSessionResult::Disposition::RefusedDescriptorConflict:
      return "refused-descriptor-conflict";
    case OpenSessionResult::Disposition::RefusedInvalid:
      return "refused-invalid";
    case OpenSessionResult::Disposition::RefusedCapacity:
      return "refused-capacity";
    case OpenSessionResult::Disposition::RefusedPersistence:
      return "refused-persistence";
    case OpenSessionResult::Disposition::RefusedClosed:
      return "refused-closed";
  }
  return "refused-invalid";
}

Registry::Registry() : impl_(std::make_unique<Impl>()) {}

Registry::~Registry() = default;

Outcome<std::unique_ptr<Registry>> Registry::Open(const RegistryOptions& options) {
  Outcome<void> limits_valid = validate_limits(options.limits);
  if (!limits_valid.has_value()) {
    return make_error<std::unique_ptr<Registry>>(limits_valid.error());
  }
  if (options.history_per_key == 0) {
    return make_error<std::unique_ptr<Registry>>(ErrorCode::InvalidArgument,
                                                 "history_per_key must be greater than zero");
  }
  if (!options.id.is_nil() && options.id.value().is_nil()) {
    return make_error<std::unique_ptr<Registry>>(ErrorCode::InvalidArgument, "registry identity is nil");
  }

  std::unique_ptr<Registry> registry(new Registry());
  registry->impl_->options = options;
  registry->impl_->limits = options.limits;
  registry->impl_->history_per_key =
      std::min(options.history_per_key, options.limits.max_history_per_key);
  registry->impl_->state.id = options.id;
  registry->impl_->state.opened_at = now_timestamp();

  if (options.store.has_value()) {
    RecoveryReport report;
    Outcome<std::unique_ptr<Store>> store = Store::Open(options.store.value(), report);
    registry->impl_->recovery = report;
    if (!store.has_value()) {
      return make_error<std::unique_ptr<Registry>>(store.error());
    }
    registry->impl_->store = std::move(store).value();

    bool replay_failed = false;
    std::string replay_error;
    Outcome<void> replayed = registry->impl_->store->VisitRecords(
        [&](RecordKind kind, std::span<const std::byte> payload) -> bool {
          if (kind == RecordKind::StateSnapshot) {
            Decoder decoder(payload);
            RegistryState loaded;
            if (!detail::decode_state(decoder, options.limits, loaded) || !decoder.done()) {
              replay_failed = true;
              replay_error = "the compacted state image could not be decoded";
              return false;
            }
            registry->impl_->state = std::move(loaded);
            registry->impl_->label_index_valid = false;
            return true;
          }
          if (kind == RecordKind::SessionOpen) {
            Decoder decoder(payload);
            SourceDescriptor descriptor;
            Incarnation incarnation;
            std::int64_t opened_at = 0;
            if (!decode_source_descriptor(decoder, options.limits, descriptor) ||
                !decoder.u64(incarnation.value) || !decoder.i64(opened_at) || !decoder.done()) {
              replay_failed = true;
              replay_error = "a session record could not be decoded";
              return false;
            }
            if (incarnation.value == 0 || descriptor.id.is_nil()) {
              replay_failed = true;
              replay_error = "a session record carries an invalid incarnation";
              return false;
            }
            apply_session_open(registry->impl_->state, descriptor, incarnation, Timestamp{opened_at});
            return true;
          }
          if (kind == RecordKind::SessionClose) {
            Decoder decoder(payload);
            SourceIncarnationKey key;
            if (!decoder.id128(key.source) || !decoder.u64(key.incarnation.value) || !decoder.done()) {
              replay_failed = true;
              replay_error = "a session close record could not be decoded";
              return false;
            }
            const auto session = registry->impl_->state.sessions.find(key);
            if (session != registry->impl_->state.sessions.end()) {
              session->second.closed = true;
              session->second.live = false;
            }
            return true;
          }
          if (kind == RecordKind::Evidence) {
            Decoder decoder(payload);
            std::uint32_t envelope_version = 0;
            Timestamp received_at;
            Evidence evidence;
            if (!decoder.u32(envelope_version) || envelope_version != 1 ||
                !decoder.i64(received_at.unix_nanos) ||
                !decode_evidence(decoder, options.limits, evidence) || !decoder.done()) {
              replay_failed = true;
              replay_error = "an evidence record could not be decoded";
              return false;
            }
            registry->impl_->ingest_locked(evidence, received_at, false, false);
            return true;
          }
          replay_failed = true;
          replay_error = "the store contains a record kind this version does not understand";
          return false;
        });
    if (replay_failed) {
      registry->impl_->store->Close();
      registry->impl_->store.reset();
      registry->impl_->recovery.opened = false;
      registry->impl_->recovery.chain_intact = false;
      registry->impl_->recovery.detail = replay_error;
      return make_error<std::unique_ptr<Registry>>(ErrorCode::CorruptStore, replay_error);
    }
    if (!replayed.has_value()) {
      registry->impl_->store->Close();
      registry->impl_->store.reset();
      return make_error<std::unique_ptr<Registry>>(replayed.error());
    }

    if (!registry->impl_->state.id.is_nil() && !options.id.is_nil() &&
        !(registry->impl_->state.id == options.id)) {
      registry->impl_->store->Close();
      registry->impl_->store.reset();
      return make_error<std::unique_ptr<Registry>>(
          ErrorCode::Conflict,
          "the store belongs to registry " + registry->impl_->state.id.to_hex() +
              " but " + options.id.to_hex() + " was requested");
    }
    if (registry->impl_->state.id.is_nil()) {
      registry->impl_->state.id = options.id.is_nil() ? RegistryId(detail::random_id128()) : options.id;
    }
    if (report.created) {
      Encoder encoder;
      RegistryState bootstrap = registry->impl_->state;
      detail::clear_volatile(bootstrap);
      detail::encode_state(encoder, bootstrap, options.limits);
      if (!encoder.ok()) {
        return make_error<std::unique_ptr<Registry>>(ErrorCode::Internal,
                                                     "the initial state image could not be encoded");
      }
      Outcome<void> written =
          registry->impl_->store->Append(RecordKind::StateSnapshot, encoder.view());
      if (!written.has_value()) {
        return make_error<std::unique_ptr<Registry>>(written.error());
      }
    }
  } else if (registry->impl_->state.id.is_nil()) {
    registry->impl_->state.id = RegistryId(detail::random_id128());
  }

  detail::clear_volatile(registry->impl_->state);
  return registry;
}

RegistryId Registry::id() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->state.id;
}

Limits Registry::limits() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->limits;
}

RecoveryReport Registry::recovery_report() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->recovery;
}

bool Registry::closed() const noexcept {
  std::shared_lock lock(impl_->mutex);
  return impl_->closed;
}

Timestamp Registry::opened_at() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->state.opened_at;
}

Outcome<OpenSessionResult> Registry::OpenSession(const SourceDescriptor& descriptor, Incarnation incarnation) {
  std::unique_lock lock(impl_->mutex);
  OpenSessionResult result;
  if (impl_->closed) {
    result.disposition = OpenSessionResult::Disposition::RefusedClosed;
    result.detail = "the registry is stopping";
    return result;
  }
  if (descriptor.id.is_nil() || incarnation.value == 0) {
    result.disposition = OpenSessionResult::Disposition::RefusedInvalid;
    result.detail = "the source identity and incarnation must both be set";
    return result;
  }
  if (!fits(descriptor.name, impl_->limits.max_string_bytes) ||
      !fits(descriptor.administrative_domain, impl_->limits.max_string_bytes)) {
    result.disposition = OpenSessionResult::Disposition::RefusedInvalid;
    result.detail = "a source descriptor string exceeds the configured limit";
    return result;
  }
  if (descriptor.authority.authority_class > AuthorityClass::Authoritative) {
    result.disposition = OpenSessionResult::Disposition::RefusedInvalid;
    result.detail = "the authority class is not recognised";
    return result;
  }

  const auto existing_source = impl_->state.sources.find(descriptor.id);
  const bool new_source = existing_source == impl_->state.sources.end();
  if (new_source && impl_->state.sources.size() >= impl_->limits.max_sources) {
    result.disposition = OpenSessionResult::Disposition::RefusedCapacity;
    result.detail = "the registry has reached its source limit";
    return result;
  }

  Incarnation previous{};
  if (!new_source) {
    previous = existing_source->second.current_incarnation;
    if (incarnation < previous) {
      result.disposition = OpenSessionResult::Disposition::RefusedFenced;
      result.previous_incarnation = previous;
      result.detail = "incarnation " + std::to_string(incarnation.value) + " is fenced by " +
                      std::to_string(previous.value);
      return result;
    }
    if (incarnation == previous && existing_source->second.described &&
        !(existing_source->second.descriptor == descriptor)) {
      result.disposition = OpenSessionResult::Disposition::RefusedDescriptorConflict;
      result.previous_incarnation = previous;
      result.detail =
          "a source's descriptor and authority are bound to its incarnation; reopen with a higher incarnation";
      return result;
    }
  }

  const SourceIncarnationKey key{descriptor.id, incarnation};
  const auto existing_session = impl_->state.sessions.find(key);
  // Captured before the session is touched: the disposition distinguishes a
  // session that is already live in this process from one recovered from
  // durable storage that has just reconnected.
  const bool was_live = existing_session != impl_->state.sessions.end() && existing_session->second.live;

  if (impl_->store) {
    Encoder encoder;
    encode_source_descriptor(encoder, descriptor, impl_->limits);
    encoder.u64(incarnation.value);
    encoder.i64(now_timestamp().unix_nanos);
    if (!encoder.ok()) {
      result.disposition = OpenSessionResult::Disposition::RefusedInvalid;
      result.detail = "the session record could not be encoded";
      return result;
    }
    Outcome<void> written = impl_->store->Append(RecordKind::SessionOpen, encoder.view());
    if (!written.has_value()) {
      result.disposition = OpenSessionResult::Disposition::RefusedPersistence;
      result.detail = written.error().message();
      return result;
    }
  }

  if (incarnation > previous) {
    result.fenced_claims = count_claims_before(impl_->state, descriptor.id, incarnation);
  }
  apply_session_open(impl_->state, descriptor, incarnation, now_timestamp());
  impl_->state.sessions[key].live = true;
  impl_->state.sessions[key].closed = false;
  impl_->state.sources[descriptor.id].current_incarnation =
      std::max(impl_->state.sources[descriptor.id].current_incarnation, incarnation);

  result.key = key;
  result.previous_incarnation = previous;
  if (new_source || previous.value == 0) {
    result.disposition = OpenSessionResult::Disposition::Opened;
    result.detail = "the source was registered";
  } else if (incarnation > previous) {
    result.disposition = OpenSessionResult::Disposition::NewIncarnation;
    result.detail = "incarnation " + std::to_string(incarnation.value) + " replaced " +
                    std::to_string(previous.value) + "; the earlier incarnation is fenced";
  } else if (was_live) {
    result.disposition = OpenSessionResult::Disposition::Resumed;
    result.detail = "the session was already live in this registry lifetime";
  } else {
    result.disposition = OpenSessionResult::Disposition::Reopened;
    result.detail =
        "the incarnation was recovered from durable storage; its claims stay historical until re-asserted";
  }
  return result;
}

Outcome<void> Registry::CloseSession(SourceIncarnationKey key) {
  std::unique_lock lock(impl_->mutex);
  if (impl_->closed) {
    return make_error(ErrorCode::Closed, "the registry is stopping");
  }
  const auto session = impl_->state.sessions.find(key);
  if (session == impl_->state.sessions.end()) {
    return make_error(ErrorCode::NotFound, "no session is open for that source incarnation");
  }
  if (impl_->store) {
    Encoder encoder;
    encoder.id128(key.source.value());
    encoder.u64(key.incarnation.value);
    Outcome<void> written = impl_->store->Append(RecordKind::SessionClose, encoder.view());
    if (!written.has_value()) {
      return make_error(ErrorCode::PersistenceFailure, written.error().message());
    }
  }
  session->second.live = false;
  session->second.closed = true;
  return Outcome<void>();
}

std::vector<SourceSessionView> Registry::Sessions() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<SourceSessionView> views;
  views.reserve(impl_->state.sessions.size());
  for (const auto& entry : impl_->state.sessions) {
    SourceSessionView view;
    view.key = entry.first;
    view.live = entry.second.live;
    view.fenced = detail::session_is_fenced(impl_->state, entry.first);
    if (view.fenced) {
      const auto source = impl_->state.sources.find(entry.first.source);
      if (source != impl_->state.sources.end()) {
        view.fenced_by = source->second.current_incarnation;
      }
    }
    view.high_water = entry.second.high_water;
    view.opened_at = entry.second.opened_at;
    view.accepted_records = entry.second.accepted;
    view.refused_records = entry.second.refused;
    view.duplicate_records = entry.second.duplicates;
    view.missing_generations = entry.second.missing_generations;
    const auto source = impl_->state.sources.find(entry.first.source);
    if (source != impl_->state.sources.end()) {
      view.name = source->second.descriptor.name;
      view.authority = source->second.descriptor.authority;
    }
    views.push_back(std::move(view));
  }
  return views;
}

Outcome<IngestResult> Registry::Ingest(const Evidence& evidence) {
  std::unique_lock lock(impl_->mutex);
  return impl_->ingest_locked(evidence, now_timestamp(), true, true);
}

std::vector<IngestResult> Registry::IngestBatch(std::span<const Evidence> evidence) {
  std::unique_lock lock(impl_->mutex);
  const std::size_t take = std::min<std::size_t>(evidence.size(), impl_->limits.max_evidence_batch);
  std::vector<IngestResult> results;
  results.reserve(take);
  for (std::size_t index = 0; index < take; ++index) {
    results.push_back(impl_->ingest_locked(evidence[index], now_timestamp(), true, true));
  }
  return results;
}


Outcome<PortView> Registry::QueryPort(PortRef port, ValidationPolicy policy) const {
  std::shared_lock lock(impl_->mutex);
  if (port.endpoint.is_nil()) {
    return make_error<PortView>(ErrorCode::InvalidArgument, "the port names a nil endpoint");
  }
  if (!detail::endpoint_exists(impl_->state, port.endpoint)) {
    return make_error<PortView>(ErrorCode::NotFound, "the endpoint is not registered");
  }
  if (!detail::port_exists(impl_->state, port)) {
    return make_error<PortView>(ErrorCode::NotFound, "the port index is outside the endpoint");
  }
  return derive_port_view(impl_->state, impl_->limits, port, policy);
}

Outcome<EndpointView> Registry::QueryEndpoint(EndpointId endpoint, bool include_ports, ValidationPolicy policy) const {
  std::shared_lock lock(impl_->mutex);
  if (endpoint.is_nil()) {
    return make_error<EndpointView>(ErrorCode::InvalidArgument, "the endpoint identity is nil");
  }
  const Folded<EndpointRegistrationValue>* registration = winning_endpoint(impl_->state, endpoint);
  if (registration == nullptr) {
    return make_error<EndpointView>(ErrorCode::NotFound, "the endpoint is not registered");
  }
  EndpointView view;
  view.id = endpoint;
  view.kind = registration->value.descriptor.kind;
  view.name = registration->value.descriptor.name;
  view.administrative_location = registration->value.descriptor.administrative_location;
  view.port_count = registration->value.descriptor.port_count;
  view.default_port_capability = registration->value.descriptor.default_port_capability;
  if (include_ports) {
    for (std::uint32_t index = 0; index < view.port_count; ++index) {
      PortRef port;
      port.endpoint = endpoint;
      port.index = PortIndex{index};
      view.ports.push_back(derive_port_view(impl_->state, impl_->limits, port, policy));
    }
  }
  return view;
}

Outcome<ObjectView> Registry::QueryObject(ObjectId object, const QueryOptions& options) const {
  std::shared_lock lock(impl_->mutex);
  if (object.is_nil()) {
    return make_error<ObjectView>(ErrorCode::InvalidArgument, "the object identity is nil");
  }
  if (!detail::object_exists(impl_->state, object)) {
    return make_error<ObjectView>(ErrorCode::NotFound, "the object is not registered");
  }
  return derive_object_view(impl_->state, impl_->limits, object, options, impl_->history_per_key);
}

Outcome<TopologyView> Registry::QueryTopology(const QueryOptions& options) const {
  std::shared_lock lock(impl_->mutex);
  TopologyView view;
  view.registry = impl_->state.id;
  view.validation_policy = options.validation_policy;

  for (const auto& entry : impl_->state.object_registrations) {
    if (!options.object.is_nil() && !(options.object == entry.first)) {
      continue;
    }
    view.objects.push_back(
        derive_object_view(impl_->state, impl_->limits, entry.first, options, impl_->history_per_key));
    if (view.objects.size() >= impl_->limits.max_snapshot_entries) {
      break;
    }
  }
  for (const auto& entry : impl_->state.endpoint_registrations) {
    if (!options.endpoint.is_nil() && !(options.endpoint == entry.first)) {
      continue;
    }
    Outcome<EndpointView> endpoint = QueryEndpoint(entry.first, options.include_endpoint_ports, options.validation_policy);
    if (!endpoint.has_value()) {
      continue;
    }
    view.endpoints.push_back(std::move(endpoint).value());
    if (view.endpoints.size() >= impl_->limits.max_snapshot_entries) {
      break;
    }
  }
  if (options.object.is_nil()) {
    for (const auto& entry : impl_->state.port_records) {
      if (!options.endpoint.is_nil() && !(options.endpoint == entry.first.endpoint)) {
        continue;
      }
      view.ports.push_back(derive_port_view(impl_->state, impl_->limits, entry.first, options.validation_policy));
      if (view.ports.size() >= impl_->limits.max_snapshot_entries) {
        break;
      }
    }
  }
  view.summary = summarize(view.ports, view.objects, count_fenced_claims(impl_->state),
                           count_pending_claims(impl_->state));
  return view;
}

Outcome<ObjectHistory> Registry::QueryHistory(ObjectId object, std::uint32_t max_events) const {
  std::shared_lock lock(impl_->mutex);
  if (object.is_nil()) {
    return make_error<ObjectHistory>(ErrorCode::InvalidArgument, "the object identity is nil");
  }
  if (!detail::object_exists(impl_->state, object)) {
    return make_error<ObjectHistory>(ErrorCode::NotFound, "the object is not registered");
  }
  ObjectHistory history;
  history.object = object;
  std::uint32_t dropped = 0;
  history.events =
      collect_history(impl_->state, object, impl_->limits, std::min(max_events, impl_->limits.max_history_events),
                      dropped);
  history.dropped_events = dropped;
  return history;
}

std::vector<PortView> Registry::QueryPortsByState(PortAttachmentState state, ValidationPolicy policy) const {
  std::shared_lock lock(impl_->mutex);
  return collect_ports_by_state(impl_->state, impl_->limits, state, policy);
}

Outcome<InspectionView> Registry::Inspect(std::uint32_t max_entries) const {
  std::shared_lock lock(impl_->mutex);
  InspectionView view;
  const std::uint32_t bound = std::min(max_entries, impl_->limits.max_snapshot_entries);

  for (const auto& entry : impl_->state.sessions) {
    if (!detail::session_is_fenced(impl_->state, entry.first)) {
      continue;
    }
    const auto source = impl_->state.sources.find(entry.first.source);
    FencedSessionView fenced;
    fenced.source = entry.first.source;
    fenced.incarnation = entry.first.incarnation;
    fenced.fenced_by = source == impl_->state.sources.end() ? Incarnation{} : source->second.current_incarnation;
    fenced.claims = count_session_claims(impl_->state, entry.first);
    view.fenced_sessions.push_back(fenced);
    if (view.fenced_sessions.size() >= bound) {
      break;
    }
  }

  const std::map<ObjectId, std::uint64_t> fenced_by_object = fenced_claims_by_object(impl_->state);
  for (const auto& entry : fenced_by_object) {
    if (entry.second == 0) {
      continue;
    }
    FencedObjectView object;
    object.object = entry.first;
    object.current_incarnation = detail::object_incarnation_of(impl_->state, entry.first);
    object.fenced_claims = entry.second;
    const Folded<ObjectRegistrationValue>* registration = winning_registration(impl_->state, entry.first);
    if (registration != nullptr) {
      object.physical_label = registration->value.descriptor.physical_label;
    }
    view.fenced_objects.push_back(std::move(object));
    if (view.fenced_objects.size() >= bound) {
      break;
    }
  }

  for (const auto& entry : impl_->state.port_records) {
    if (!detail::object_exists(impl_->state, entry.second.begin()->second.value.subject.object)) {
      PendingReferenceView pending;
      pending.kind = EvidenceKind::Attach;
      pending.source = entry.second.begin()->first.owner.source;
      pending.incarnation = entry.second.begin()->first.owner.incarnation;
      pending.generation = entry.second.begin()->second.meta.generation;
      pending.evidence = entry.second.begin()->second.meta.evidence;
      pending.detail = "the attached object is not registered yet";
      view.pending_references.push_back(std::move(pending));
      continue;
    }
    if (!detail::port_exists(impl_->state, entry.first)) {
      PendingReferenceView pending;
      pending.kind = EvidenceKind::Attach;
      pending.source = entry.second.begin()->first.owner.source;
      pending.incarnation = entry.second.begin()->first.owner.incarnation;
      pending.generation = entry.second.begin()->second.meta.generation;
      pending.evidence = entry.second.begin()->second.meta.evidence;
      pending.detail = "the port is not registered yet";
      view.pending_references.push_back(std::move(pending));
      continue;
    }
    PortView port = derive_port_view(impl_->state, impl_->limits, entry.first, ValidationPolicy::IncludeAll);
    if (port.validation == ValidationState::Unvalidated && port.state != PortAttachmentState::Unknown) {
      view.unvalidated_ports.push_back(std::move(port));
      if (view.unvalidated_ports.size() >= bound) {
        break;
      }
    }
  }

  view.refusals.assign(impl_->refusals.begin(), impl_->refusals.end());
  const std::vector<PortView> all_ports = [&] {
    std::vector<PortView> ports;
    for (const auto& entry : impl_->state.port_records) {
      ports.push_back(derive_port_view(impl_->state, impl_->limits, entry.first, ValidationPolicy::IncludeAll));
      if (ports.size() >= bound) {
        break;
      }
    }
    return ports;
  }();
  std::vector<ObjectView> objects;
  for (const auto& entry : impl_->state.object_registrations) {
    QueryOptions options;
    options.include_object_sides = false;
    objects.push_back(derive_object_view(impl_->state, impl_->limits, entry.first, options, impl_->history_per_key));
    if (objects.size() >= bound) {
      break;
    }
  }
  view.summary =
      summarize(all_ports, objects, count_fenced_claims(impl_->state), count_pending_claims(impl_->state));
  return view;
}

Outcome<TopologySnapshot> Registry::Snapshot() const {
  std::shared_lock lock(impl_->mutex);
  return build_snapshot(impl_->state, impl_->limits);
}

Outcome<Digest256> Registry::GraphDigest() const {
  std::shared_lock lock(impl_->mutex);
  return build_snapshot(impl_->state, impl_->limits).graph_digest();
}

Outcome<SnapshotEnvelope> Registry::SnapshotWithMetadata() const {
  std::shared_lock lock(impl_->mutex);
  SnapshotEnvelope envelope;
  envelope.snapshot = build_snapshot(impl_->state, impl_->limits);
  envelope.digest = envelope.snapshot.digest();
  envelope.generated_at = now_timestamp();
  for (const auto& entry : impl_->state.port_records) {
    const PortView port = derive_port_view(impl_->state, impl_->limits, entry.first, ValidationPolicy::IncludeAll);
    switch (port.validation) {
      case ValidationState::Validated:
        ++envelope.validation.ports_validated;
        break;
      case ValidationState::PartiallyValidated:
        ++envelope.validation.ports_partially_validated;
        break;
      case ValidationState::Unvalidated:
        ++envelope.validation.ports_unvalidated;
        break;
    }
    for (const AttachmentEdge& edge : port.edges) {
      for (const ClaimProvenance& claim : edge.claims) {
        if (claim.asserted_in_lifetime) {
          ++envelope.validation.claims_live;
        } else {
          ++envelope.validation.claims_recovered;
        }
      }
    }
  }
  return envelope;
}

RegistryStats Registry::Stats() const {
  std::shared_lock lock(impl_->mutex);
  return build_stats(impl_->state, impl_->recovery, impl_->store.get());
}

Outcome<void> Registry::Compact() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->closed) {
    return make_error(ErrorCode::Closed, "the registry is stopping");
  }
  if (!impl_->store) {
    return make_error(ErrorCode::InvalidArgument, "the registry has no store to compact");
  }
  RegistryState image = impl_->state;
  detail::clear_volatile(image);
  Encoder encoder;
  detail::encode_state(encoder, image, impl_->limits);
  if (!encoder.ok()) {
    return make_error(ErrorCode::CapacityExceeded, "the state image exceeds the encoder ceiling");
  }
  std::vector<StoreRecord> records;
  StoreRecord record;
  record.kind = RecordKind::StateSnapshot;
  record.payload = encoder.take();
  records.push_back(std::move(record));
  Outcome<void> rewritten = impl_->store->Rewrite(records);
  if (!rewritten.has_value()) {
    return make_error(ErrorCode::PersistenceFailure, rewritten.error().message());
  }
  return Outcome<void>();
}

void Registry::Close() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->closed) {
    return;
  }
  impl_->closed = true;
  if (impl_->store) {
    impl_->store->Close();
  }
}

} // namespace cable_registry
