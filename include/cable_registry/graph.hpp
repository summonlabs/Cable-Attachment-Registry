// Cable Attachment Registry — authoritative attachment graph views.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_GRAPH_HPP
#define CABLE_REGISTRY_GRAPH_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "cable_registry/authority.hpp"
#include "cable_registry/capability.hpp"
#include "cable_registry/evidence.hpp"
#include "cable_registry/ids.hpp"
#include "cable_registry/identity.hpp"
#include "cable_registry/time.hpp"

namespace cable_registry {

/// Whether the claims behind an answer are still being asserted by a live
/// source, or were merely recovered from durable storage.
///
/// Recovered evidence is never silently promoted to fresh. A registry that
/// restarted answers with Unvalidated until the owning source re-asserts the
/// claim at a higher generation.
enum class ValidationState : std::uint8_t {
  /// Nothing behind this answer was asserted in this registry lifetime.
  Unvalidated = 0,
  /// Some of the winning claims were re-asserted in this lifetime.
  PartiallyValidated,
  /// Every winning claim was asserted in this lifetime by a live source.
  Validated,
};

CABLE_REGISTRY_API const char* to_string(ValidationState value) noexcept;

/// Whether unvalidated claims participate in an answer.
enum class ValidationPolicy : std::uint8_t {
  /// Report every accepted claim and tag its validation state. The default.
  IncludeAll = 0,
  /// Drop claims that were not asserted in this registry lifetime. A port
  /// whose only claim is recovered becomes UNKNOWN rather than attached.
  LiveOnly,
};

CABLE_REGISTRY_API const char* to_string(ValidationPolicy value) noexcept;

/// Why a port or an object side could not be resolved to a single state.
enum class ConflictReason : std::uint8_t {
  None = 0,
  /// More distinct subjects claim the port than its multi-attachment
  /// capability admits.
  TooManyAttachments,
  /// Equal authority asserts both that the port is empty and that it is
  /// occupied.
  EmptyAndOccupied,
  /// One object side is claimed at two different ports.
  ObjectSideDoubleBooked,
  /// Two sources at the same authority assert different lifecycle states.
  LifecycleConflict,
  /// Two sources at the same authority declare different port capabilities.
  CapabilityConflict,
  /// A connector declaration mismatch between the object side and the port.
  /// Reported, while the attachment is still reported: an observation beats a
  /// declaration.
  IncompatibleConnector,
  IncompatibleMedia,
};

CABLE_REGISTRY_API const char* to_string(ConflictReason value) noexcept;

/// State of one port in the authoritative graph.
enum class PortAttachmentState : std::uint8_t {
  /// Nothing has ever been asserted about this port. Distinct from Empty,
  /// which means a source asserted the port was vacated.
  Unknown = 0,
  /// A source at the winning authority asserts the port is empty.
  Empty,
  /// As many subjects as the declared capability admits occupy the port.
  Attached,
  /// The claims cannot be reconciled. Every candidate is reported; none is
  /// chosen.
  Conflicting,
};

CABLE_REGISTRY_API const char* to_string(PortAttachmentState value) noexcept;

/// The provenance of one claim that participates in an answer.
struct ClaimProvenance {
  SourceId source{};
  Incarnation incarnation{};
  Generation generation{};
  EvidenceId evidence{};
  Timestamp observed_at{};
  Timestamp received_at{};
  ProvenanceClass provenance = ProvenanceClass::Unknown;
  Authority authority{};
  ObjectIncarnation object_incarnation{};
  /// True when the record was accepted or re-asserted while its source
  /// session was live in this registry lifetime.
  bool asserted_in_lifetime = false;

  friend bool operator==(const ClaimProvenance&, const ClaimProvenance&) noexcept = default;
};

/// One explanation of an unresolved situation. Claims are always cited: a
/// conflict is never reported as a bare boolean.
struct ConflictNote {
  ConflictReason reason = ConflictReason::None;
  /// Subject the note is about, when the conflict is subject local.
  ObjectSideRef subject{};
  /// Port the note is about, when the conflict is port local.
  PortRef port{};
  std::string detail;
  std::vector<ClaimProvenance> claims;

  friend bool operator==(const ConflictNote&, const ConflictNote&) noexcept = default;
};

/// One resolved or unresolved attachment of one object side to one port.
struct AttachmentEdge {
  ObjectSideRef subject{};
  PortRef port{};
  AttachmentSlot slot{};
  Authority authority{};
  ValidationState validation = ValidationState::Unvalidated;
  bool connector_compatible = true;
  bool media_compatible = true;
  bool conflicting = false;
  ObjectIncarnation object_incarnation{};
  /// Corroborating winning claims, ordered by (source, incarnation, generation).
  std::vector<ClaimProvenance> claims;
  /// Claims at a strictly lower authority. Retained as history and counted,
  /// even when the list is truncated by Limits::max_overridden_reported.
  std::vector<ClaimProvenance> overridden;
  std::uint32_t overridden_total = 0;

  friend bool operator==(const AttachmentEdge&, const AttachmentEdge&) noexcept = default;
};

/// The authoritative answer for one port.
struct PortView {
  PortRef port{};
  PortAttachmentState state = PortAttachmentState::Unknown;
  ValidationState validation = ValidationState::Unvalidated;
  PortCapability capability{};
  /// Present when state is Attached or Conflicting.
  std::vector<AttachmentEdge> edges;
  std::vector<ConflictNote> conflicts;
  /// Claims that assert the port is, or is not, empty.
  std::vector<ClaimProvenance> emptiness_claims;
  std::uint32_t emptiness_claims_total = 0;

  friend bool operator==(const PortView&, const PortView&) noexcept = default;
};

/// The authoritative answer for one connector position of one object.
struct ObjectSideView {
  SideIndex side{};
  ObjectSideDescriptor descriptor{};
  PortAttachmentState state = PortAttachmentState::Unknown;
  ValidationState validation = ValidationState::Unvalidated;
  std::vector<AttachmentEdge> edges;
  std::vector<ConflictNote> conflicts;

  friend bool operator==(const ObjectSideView&, const ObjectSideView&) noexcept = default;
};

/// One lifecycle transition in an object's history.
struct LifecycleEvent {
  LifecycleEventKind kind = LifecycleEventKind::Registered;
  Timestamp observed_at{};
  Timestamp received_at{};
  SourceId source{};
  Incarnation incarnation{};
  Generation generation{};
  EvidenceId evidence{};
  ProvenanceClass provenance = ProvenanceClass::Unknown;
  ObjectIncarnation object_incarnation{};
  SideIndex side{};
  PortRef port{};
  PortRef from_port{};
  bool has_from_port = false;
  std::string detail;

  friend bool operator==(const LifecycleEvent&, const LifecycleEvent&) noexcept = default;
};

/// The authoritative answer for one object.
struct ObjectView {
  ObjectId id{};
  ObjectKind kind = ObjectKind::Unknown;
  std::string physical_label;
  std::string serial_like;
  std::string administrative_location;
  ObjectIncarnation incarnation{};
  LifecycleState lifecycle = LifecycleState::Registered;
  NominalCapability capability{};
  std::vector<ObjectSideView> sides;
  std::vector<LifecycleEvent> history;
  std::uint32_t history_dropped = 0;
  std::vector<ConflictNote> conflicts;
  ValidationState validation = ValidationState::Unvalidated;

  friend bool operator==(const ObjectView&, const ObjectView&) noexcept = default;
};

/// The authoritative answer for one endpoint.
struct EndpointView {
  EndpointId id{};
  EndpointKind kind = EndpointKind::Unknown;
  std::string name;
  std::string administrative_location;
  std::uint32_t port_count = 0;
  PortCapability default_port_capability{};
  /// Present only when the query asked for port detail.
  std::vector<PortView> ports;

  friend bool operator==(const EndpointView&, const EndpointView&) noexcept = default;
};

/// Aggregate counts over one answer. Every field is a pure function of the
/// accepted evidence, so two registries that accepted the same records agree.
struct GraphSummary {
  std::uint64_t ports_attached = 0;
  std::uint64_t ports_empty = 0;
  std::uint64_t ports_unknown = 0;
  std::uint64_t ports_conflicting = 0;
  std::uint64_t objects_total = 0;
  std::uint64_t objects_registered = 0;
  std::uint64_t objects_unattached = 0;
  std::uint64_t objects_attached = 0;
  std::uint64_t objects_quarantined = 0;
  std::uint64_t objects_removed = 0;
  std::uint64_t objects_retired = 0;
  std::uint64_t edges = 0;
  std::uint64_t edges_unvalidated = 0;
  std::uint64_t conflicts = 0;
  std::uint64_t claims_fenced = 0;
  std::uint64_t claims_pending = 0;

  friend bool operator==(const GraphSummary&, const GraphSummary&) noexcept = default;
};

/// Options for a graph query.
struct QueryOptions {
  ValidationPolicy validation_policy = ValidationPolicy::IncludeAll;
  /// Include the per side detail in every ObjectView.
  bool include_object_sides = true;
  /// Include the lifecycle history in every ObjectView.
  bool include_history = false;
  /// Include the per port detail of every endpoint.
  bool include_endpoint_ports = false;
  /// Restrict the answer to one endpoint. A nil endpoint means all.
  EndpointId endpoint{};
  /// Restrict the answer to one object. A nil object means all objects.
  ObjectId object{};
};

/// The whole authoritative graph at one instant, as a value.
struct TopologyView {
  RegistryId registry{};
  GraphSummary summary;
  ValidationPolicy validation_policy = ValidationPolicy::IncludeAll;
  std::vector<ObjectView> objects;
  std::vector<EndpointView> endpoints;
  std::vector<PortView> ports;

  friend bool operator==(const TopologyView&, const TopologyView&) noexcept = default;
};

/// One object's history answer.
struct ObjectHistory {
  ObjectId object{};
  std::vector<LifecycleEvent> events;
  /// Events the bounded history dropped.
  std::uint32_t dropped_events = 0;

  friend bool operator==(const ObjectHistory&, const ObjectHistory&) noexcept = default;
};

/// One source incarnation that has been fenced by a newer one.
struct FencedSessionView {
  SourceId source{};
  Incarnation incarnation{};
  Incarnation fenced_by{};
  std::uint64_t claims = 0;
};

/// One object whose claims were fenced by a replacement.
struct FencedObjectView {
  ObjectId object{};
  ObjectIncarnation current_incarnation{};
  std::uint64_t fenced_claims = 0;
  std::string physical_label;
};

/// One evidence record whose reference does not exist yet.
struct PendingReferenceView {
  EvidenceId evidence{};
  EvidenceKind kind = EvidenceKind::RegisterObject;
  SourceId source{};
  Incarnation incarnation{};
  Generation generation{};
  std::string detail;
};

/// One refusal retained for inspection.
struct RefusalView {
  EvidenceId evidence{};
  EvidenceKind kind = EvidenceKind::RegisterObject;
  SourceId source{};
  Incarnation incarnation{};
  Generation generation{};
  IngestDisposition disposition = IngestDisposition::RefusedInvalid;
  std::string detail;
};

/// Everything a caller needs to answer "what is unresolved here".
struct InspectionView {
  std::vector<FencedSessionView> fenced_sessions;
  std::vector<FencedObjectView> fenced_objects;
  std::vector<PortView> unvalidated_ports;
  std::vector<PendingReferenceView> pending_references;
  std::vector<RefusalView> refusals;
  GraphSummary summary;

  friend bool operator==(const InspectionView&, const InspectionView&) noexcept = default;
};

} // namespace cable_registry

#endif // CABLE_REGISTRY_GRAPH_HPP
