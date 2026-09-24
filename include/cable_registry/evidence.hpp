// Cable Attachment Registry — evidence records and typed ingest outcomes.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_EVIDENCE_HPP
#define CABLE_REGISTRY_EVIDENCE_HPP

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "cable_registry/authority.hpp"
#include "cable_registry/capability.hpp"
#include "cable_registry/ids.hpp"
#include "cable_registry/identity.hpp"
#include "cable_registry/time.hpp"
#include "cable_registry/version.hpp"

namespace cable_registry {

/// The kind of fact an evidence record carries.
enum class EvidenceKind : std::uint16_t {
  RegisterObject = 1,
  RegisterEndpoint = 2,
  Attach = 3,
  Detach = 4,
  Move = 5,
  PublishObjectCapability = 6,
  PublishPortCapability = 7,
  SetLifecycle = 8,
  ReincarnateObject = 9,
};

CABLE_REGISTRY_API const char* to_string(EvidenceKind value) noexcept;
CABLE_REGISTRY_API bool parse_evidence_kind(std::string_view text, EvidenceKind& out) noexcept;

/// Registration of a physical object, or an idempotent restatement of one.
///
/// Two registrations of the same identity whose content differs are a
/// conflict: the registry keeps the first and reports the second as refused
/// unless it is a byte-for-byte restatement.
struct RegisterObjectPayload {
  ObjectDescriptor descriptor;
  friend bool operator==(const RegisterObjectPayload&, const RegisterObjectPayload&) noexcept = default;
};

/// Registration of an endpoint and its port count.
struct RegisterEndpointPayload {
  EndpointDescriptor descriptor;
  friend bool operator==(const RegisterEndpointPayload&, const RegisterEndpointPayload&) noexcept = default;
};

/// "This side of this object is plugged into this port."
///
/// object_incarnation binds the claim to the physical unit it was observed on.
/// A claim recorded against a superseded incarnation is fenced and can never
/// bind to the replacement.
struct AttachPayload {
  ObjectSideRef subject{};
  PortRef port{};
  ObjectIncarnation object_incarnation{};
  /// Slot inside a multi-attachment port. Must be zero on a port that declares
  /// one attachment.
  AttachmentSlot slot{};
  friend bool operator==(const AttachPayload&, const AttachPayload&) noexcept = default;
};

/// "This side of this object is not plugged in."
///
/// When from_port is set the source also asserts that the named port is now
/// empty, which is what lets a port distinguish EMPTY from never observed.
struct DetachPayload {
  ObjectSideRef subject{};
  ObjectIncarnation object_incarnation{};
  bool has_from_port = false;
  PortRef from_port{};
  /// Slot vacated at from_port. Only meaningful when has_from_port is set.
  AttachmentSlot slot{};
  friend bool operator==(const DetachPayload&, const DetachPayload&) noexcept = default;
};

/// "This side of this object moved from one port to another."
///
/// A move is one atomic fact, not a detach that happens to be followed by an
/// attach: the registry applies it as a single unit so that no observer can
/// see the intermediate state.
struct MovePayload {
  ObjectSideRef subject{};
  ObjectIncarnation object_incarnation{};
  PortRef from{};
  PortRef to{};
  AttachmentSlot from_slot{};
  AttachmentSlot to_slot{};
  friend bool operator==(const MovePayload&, const MovePayload&) noexcept = default;
};

/// Replaces the nominal capability of an object.
struct PublishObjectCapabilityPayload {
  ObjectId object{};
  NominalCapability capability;
  friend bool operator==(const PublishObjectCapabilityPayload&, const PublishObjectCapabilityPayload&) noexcept = default;
};

/// Replaces the capability and multi-attachment declaration of one port.
struct PublishPortCapabilityPayload {
  PortRef port{};
  PortCapability capability;
  friend bool operator==(const PublishPortCapabilityPayload&, const PublishPortCapabilityPayload&) noexcept = default;
};

/// Asserts the lifecycle state of an object.
struct SetLifecyclePayload {
  ObjectId object{};
  ObjectIncarnation object_incarnation{};
  LifecycleAction action = LifecycleAction::Present;
  std::string reason;
  friend bool operator==(const SetLifecyclePayload&, const SetLifecyclePayload&) noexcept = default;
};

/// Installs a replacement physical unit under the same asset identity.
///
/// from_incarnation must be the incarnation the source believes is current.
/// The registry folds reincarnation as a maximum over target incarnations, so
/// the resulting incarnation does not depend on the order the records arrived
/// in. Every claim recorded against an older incarnation is fenced.
struct ReincarnateObjectPayload {
  ObjectId object{};
  ObjectIncarnation from_incarnation{};
  /// Serial-like identity of the replacement unit, when it is known.
  std::string new_serial_like;
  std::string reason;
  friend bool operator==(const ReincarnateObjectPayload&, const ReincarnateObjectPayload&) noexcept = default;
};

using EvidencePayload = std::variant<RegisterObjectPayload,
                                     RegisterEndpointPayload,
                                     AttachPayload,
                                     DetachPayload,
                                     MovePayload,
                                     PublishObjectCapabilityPayload,
                                     PublishPortCapabilityPayload,
                                     SetLifecyclePayload,
                                     ReincarnateObjectPayload>;

CABLE_REGISTRY_API EvidenceKind kind_of(const EvidencePayload& payload) noexcept;

/// The attribution and ordering envelope of one evidence record.
struct EvidenceHeader {
  /// Unique per record. A record whose (source, incarnation, generation, key)
  /// matches an already stored record is a duplicate only when its content
  /// matches too; otherwise it is refused as a generation collision.
  EvidenceId id{};
  SourceId source{};
  /// Incarnation of the publishing process. Must match an open session.
  Incarnation incarnation{};
  /// Monotonic inside the incarnation. The registry orders one source's claims
  /// by this value and never by arrival time.
  Generation generation{};
  /// When the source observed the fact.
  Timestamp observed_at{};
  ProvenanceClass provenance = ProvenanceClass::Unknown;
  /// Free form detail, bounded by Limits::max_string_bytes.
  std::string provenance_detail;
  std::uint32_t schema_version = kEvidenceSchemaVersion;

  friend bool operator==(const EvidenceHeader&, const EvidenceHeader&) noexcept = default;
};

/// One accepted unit of physical evidence.
struct Evidence {
  EvidenceHeader header;
  EvidencePayload payload;

  friend bool operator==(const Evidence&, const Evidence&) noexcept = default;
};

CABLE_REGISTRY_API EvidenceKind kind_of(const Evidence& evidence) noexcept;

/// Mints a fresh evidence identifier from the process entropy source.
CABLE_REGISTRY_API EvidenceId new_evidence_id() noexcept;

/// What the registry did with one evidence record.
///
/// The outcome of an ingest call describes that event against the state at the
/// time it was applied. The canonical attachment graph, in contrast, is a pure
/// function of the accepted records and does not depend on the order they
/// arrived in.
enum class IngestDisposition : std::uint8_t {
  /// Recorded, and effective in the authoritative graph right now.
  Accepted = 0,
  /// Recorded, currently fenced by a newer incarnation of the same source. It
  /// can never bind again.
  AcceptedFencedSource,
  /// Recorded, currently fenced by a newer incarnation of the physical object.
  /// It can never bind again.
  AcceptedFencedIncarnation,
  /// Recorded, but the object, endpoint or port it names is not registered
  /// yet. It becomes effective on its own once the reference is registered.
  AcceptedPendingReference,
  /// Recorded, but the object has not reached the named incarnation yet. It
  /// becomes effective on its own once a reincarnation reaches it.
  AcceptedPendingIncarnation,
  /// Recorded for history only: a strictly newer generation of the same source
  /// stream already owns the claim key.
  AcceptedSuperseded,
  /// Identical to a record already stored at the same generation.
  Duplicate,
  /// The source has no session. Open one first.
  RefusedUnknownSource,
  /// The physical label is already bound to a live object.
  RefusedLabelConflict,
  /// The same generation already carries different content.
  RefusedConflict,
  /// Malformed payload, nil identity, or a bound violation.
  RefusedInvalid,
  /// A registry limit would be exceeded.
  RefusedCapacity,
  /// The registry is shutting down and no longer accepts work.
  RefusedClosed,
  /// The record could not be made durable.
  RefusedPersistence,
};

CABLE_REGISTRY_API const char* to_string(IngestDisposition value) noexcept;

/// True when the record was durably stored, whatever its later effect is.
CABLE_REGISTRY_API bool is_accepted(IngestDisposition value) noexcept;
/// True when the record contributes to the authoritative graph right now.
CABLE_REGISTRY_API bool is_effective(IngestDisposition value) noexcept;

/// The result of one ingest call.
struct IngestResult {
  IngestDisposition disposition = IngestDisposition::RefusedInvalid;
  EvidenceId evidence{};
  EvidenceKind kind = EvidenceKind::RegisterObject;
  /// Highest generation recorded for the source incarnation after this call.
  Generation high_water{};
  /// Stamped by the registry. Provenance only; never an ordering key.
  Timestamp received_at{};
  /// Explanation, always set when the disposition is a refusal.
  std::string detail;

  [[nodiscard]] bool accepted() const noexcept { return is_accepted(disposition); }
  [[nodiscard]] bool effective() const noexcept { return is_effective(disposition); }
};

} // namespace cable_registry

#endif // CABLE_REGISTRY_EVIDENCE_HPP
