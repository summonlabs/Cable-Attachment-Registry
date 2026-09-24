// Cable Attachment Registry — immutable topology provenance snapshots.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_SNAPSHOT_HPP
#define CABLE_REGISTRY_SNAPSHOT_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cable_registry/authority.hpp"
#include "cable_registry/digest.hpp"
#include "cable_registry/error.hpp"
#include "cable_registry/evidence.hpp"
#include "cable_registry/limits.hpp"
#include "cable_registry/graph.hpp"

namespace cable_registry {

/// One claim behind a snapshot attachment. Deliberately carries no receive
/// time and no validation state: neither may influence snapshot identity.
struct SnapshotClaim {
  SourceId source{};
  Incarnation incarnation{};
  Generation generation{};
  EvidenceId evidence{};
  Timestamp observed_at{};
  ProvenanceClass provenance = ProvenanceClass::Unknown;
  Authority authority{};

  friend bool operator==(const SnapshotClaim&, const SnapshotClaim&) noexcept = default;
};

/// One attachment in a snapshot.
struct SnapshotAttachment {
  ObjectSideRef subject{};
  PortRef port{};
  AttachmentSlot slot{};
  ObjectIncarnation object_incarnation{};
  Authority authority{};
  bool connector_compatible = true;
  bool media_compatible = true;
  bool conflicting = false;
  std::vector<SnapshotClaim> claims;

  friend bool operator==(const SnapshotAttachment&, const SnapshotAttachment&) noexcept = default;
};

/// One conflict in a snapshot.
struct SnapshotConflict {
  ConflictReason reason = ConflictReason::None;
  ObjectSideRef subject{};
  PortRef port{};
  std::vector<SnapshotClaim> claims;

  friend bool operator==(const SnapshotConflict&, const SnapshotConflict&) noexcept = default;
};

/// One port in a snapshot.
struct SnapshotPort {
  PortRef port{};
  PortAttachmentState state = PortAttachmentState::Unknown;
  PortCapability capability{};
  std::vector<SnapshotAttachment> attachments;
  std::vector<SnapshotConflict> conflicts;

  friend bool operator==(const SnapshotPort&, const SnapshotPort&) noexcept = default;
};

/// One physical object in a snapshot.
struct SnapshotObject {
  ObjectId id{};
  ObjectKind kind = ObjectKind::Unknown;
  std::string physical_label;
  std::string serial_like;
  std::string administrative_location;
  ObjectIncarnation incarnation{};
  LifecycleState lifecycle = LifecycleState::Registered;
  NominalCapability capability{};
  std::vector<ObjectSideDescriptor> sides;
  std::vector<SnapshotAttachment> attachments;

  friend bool operator==(const SnapshotObject&, const SnapshotObject&) noexcept = default;
};

/// One endpoint in a snapshot.
struct SnapshotEndpoint {
  EndpointId id{};
  EndpointKind kind = EndpointKind::Unknown;
  std::string name;
  std::string administrative_location;
  std::uint32_t port_count = 0;
  PortCapability default_port_capability{};

  friend bool operator==(const SnapshotEndpoint&, const SnapshotEndpoint&) noexcept = default;
};

/// One fenced source incarnation in a snapshot.
struct SnapshotFencedSession {
  SourceId source{};
  Incarnation incarnation{};
  Incarnation fenced_by{};
  std::uint64_t claims = 0;

  friend bool operator==(const SnapshotFencedSession&, const SnapshotFencedSession&) noexcept = default;
};

/// One object whose earlier incarnation is fenced, in a snapshot.
struct SnapshotFencedObject {
  ObjectId object{};
  ObjectIncarnation current_incarnation{};
  std::uint64_t fenced_claims = 0;

  friend bool operator==(const SnapshotFencedObject&, const SnapshotFencedObject&) noexcept = default;
};

/// An immutable, canonical, self-describing image of the authoritative graph.
///
/// Snapshot identity is a pure function of the accepted evidence: two
/// registries that hold the same effective claims produce byte-identical
/// canonical encodings regardless of the order the records arrived in, the
/// wall clock, or the receive timestamps. Nothing that varies with arrival
/// order or process lifetime is encoded.
struct TopologySnapshot {
  std::uint32_t format_version = kSnapshotFormatVersion;
  RegistryId registry{};
  /// Aggregate counts over the image. Lifetime-dependent counters, such as the
  /// number of unvalidated edges, are zero: they belong to a registry session,
  /// not to the evidence, and encoding them would make two registries that
  /// hold the same evidence disagree. SnapshotEnvelope carries them instead.
  GraphSummary summary;
  std::vector<SnapshotObject> objects;
  std::vector<SnapshotEndpoint> endpoints;
  std::vector<SnapshotPort> ports;
  std::vector<SnapshotFencedSession> fenced_sessions;
  std::vector<SnapshotFencedObject> fenced_objects;
  /// SHA-256 over the effective claim set: every winning claim and every claim
  /// that was fenced or superseded, keyed by the record that produced it. Two
  /// registries that accepted the same records agree on this value even when
  /// the registry identities differ.
  Digest256 provenance_digest{};

  friend bool operator==(const TopologySnapshot&, const TopologySnapshot&) noexcept = default;

  /// The canonical encoding, including the registry identity.
  [[nodiscard]] CABLE_REGISTRY_API std::vector<std::byte> encode() const;
  /// SHA-256 of encode().
  [[nodiscard]] CABLE_REGISTRY_API Digest256 digest() const;
  /// SHA-256 of the encoding with the registry identity excluded. This is the
  /// value two independent registries compare.
  [[nodiscard]] CABLE_REGISTRY_API Digest256 graph_digest() const;
};

/// Decodes a canonical snapshot. The encoding is validated before any
/// allocation and every nested collection is bounded by @p limits.
CABLE_REGISTRY_API Outcome<TopologySnapshot> decode_snapshot(std::span<const std::byte> data, const Limits& limits);

/// Validation counts that accompany a snapshot without being part of its
/// identity. A registry that restarted reports its recovered claims here.
struct SnapshotValidationSummary {
  std::uint64_t claims_live = 0;
  std::uint64_t claims_recovered = 0;
  std::uint64_t ports_validated = 0;
  std::uint64_t ports_partially_validated = 0;
  std::uint64_t ports_unvalidated = 0;

  friend bool operator==(const SnapshotValidationSummary&, const SnapshotValidationSummary&) noexcept = default;
};

/// A snapshot together with the metadata that must not change its identity.
struct SnapshotEnvelope {
  TopologySnapshot snapshot;
  /// When this image was produced. Not part of the digest.
  Timestamp generated_at{};
  Digest256 digest{};
  SnapshotValidationSummary validation;

  friend bool operator==(const SnapshotEnvelope&, const SnapshotEnvelope&) noexcept = default;
};

} // namespace cable_registry

#endif // CABLE_REGISTRY_SNAPSHOT_HPP
