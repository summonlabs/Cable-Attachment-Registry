// Cable Attachment Registry — the registry runtime.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_REGISTRY_HPP
#define CABLE_REGISTRY_REGISTRY_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "cable_registry/authority.hpp"
#include "cable_registry/digest.hpp"
#include "cable_registry/error.hpp"
#include "cable_registry/evidence.hpp"
#include "cable_registry/graph.hpp"
#include "cable_registry/limits.hpp"
#include "cable_registry/persistence.hpp"
#include "cable_registry/snapshot.hpp"

namespace cable_registry {

/// How the registry is configured at open.
struct RegistryOptions {
  /// Registry identity. A nil value makes the registry mint one, which is then
  /// persisted with the first record.
  RegistryId id{};
  Limits limits{};
  /// Absent means the registry is in memory only and nothing survives the
  /// process. Present means every accepted record is made durable before it is
  /// acknowledged.
  std::optional<StoreOptions> store;
  /// Prior records retained per claim key for history answers.
  std::uint32_t history_per_key = 8;
};

/// What OpenSession did.
struct OpenSessionResult {
  enum class Disposition : std::uint8_t {
    /// The source was registered for the first time.
    Opened = 0,
    /// The incarnation was already the live one in this process.
    Resumed,
    /// The registry recovered this incarnation from durable storage and the
    /// source has now reconnected to it. Recovered claims stay historical
    /// until they are re-asserted at a higher generation.
    Reopened,
    /// A strictly higher incarnation replaced the previous one. Everything
    /// recorded under the previous incarnation is fenced.
    NewIncarnation,
    /// The incarnation is older than the live one. The session is not opened.
    RefusedFenced,
    /// The incarnation matches the live one but the descriptor differs. A
    /// source's authority is bound to its incarnation; changing it requires a
    /// new incarnation.
    RefusedDescriptorConflict,
    RefusedInvalid,
    RefusedCapacity,
    RefusedPersistence,
    RefusedClosed,
  };

  Disposition disposition = Disposition::RefusedInvalid;
  SourceIncarnationKey key{};
  Incarnation previous_incarnation{};
  /// Claims fenced by this call, when the disposition is NewIncarnation.
  std::uint64_t fenced_claims = 0;
  std::string detail;
};

CABLE_REGISTRY_API const char* to_string(OpenSessionResult::Disposition value) noexcept;

/// One source incarnation known to the registry.
struct SourceSessionView {
  SourceIncarnationKey key{};
  std::string name;
  Authority authority{};
  /// True when the session was opened in this registry lifetime.
  bool live = false;
  /// True when a strictly higher incarnation of the same source exists.
  bool fenced = false;
  Incarnation fenced_by{};
  Generation high_water{};
  Timestamp opened_at{};
  std::uint64_t accepted_records = 0;
  std::uint64_t refused_records = 0;
  std::uint64_t duplicate_records = 0;
  /// Generations skipped relative to the previous accepted record. Recorded,
  /// not fatal: a source may legitimately skip a value.
  std::uint64_t missing_generations = 0;

  friend bool operator==(const SourceSessionView&, const SourceSessionView&) noexcept = default;
};

/// Counter snapshot of the registry.
struct RegistryStats {
  RegistryId registry{};
  Timestamp opened_at{};
  std::uint64_t objects = 0;
  std::uint64_t endpoints = 0;
  std::uint64_t ports = 0;
  std::uint64_t sources = 0;
  std::uint64_t sessions = 0;
  std::uint64_t live_sessions = 0;
  std::uint64_t claim_records = 0;
  std::uint64_t port_records = 0;
  std::uint64_t lifecycle_records = 0;
  std::uint64_t capability_records = 0;
  std::uint64_t evidence_accepted = 0;
  std::uint64_t evidence_refused = 0;
  std::uint64_t evidence_duplicate = 0;
  std::uint64_t evidence_superseded = 0;
  std::uint64_t evidence_fenced = 0;
  std::uint64_t evidence_pending = 0;
  std::uint64_t store_records = 0;
  std::uint64_t store_bytes = 0;
  std::uint64_t store_rewrites = 0;
  RecoveryReport recovery;
};

/// The registry.
///
/// Thread safety: every public method is safe to call concurrently. Ingest
/// takes an exclusive lock, queries take a shared one, and no query upgrades
/// to a write. No caller-supplied code runs while a lock is held, so a
/// callback can never deadlock against the registry.
class CABLE_REGISTRY_API Registry {
 public:
  /// Opens a registry, recovering the store when one is configured. Fails
  /// rather than guessing when the store is damaged and the policy is strict.
  static Outcome<std::unique_ptr<Registry>> Open(const RegistryOptions& options);

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;
  ~Registry();

  [[nodiscard]] RegistryId id() const;
  [[nodiscard]] Limits limits() const;
  [[nodiscard]] RecoveryReport recovery_report() const;
  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] Timestamp opened_at() const;

  // -- control plane ------------------------------------------------------

  /// Registers a source or advances its incarnation. Fences every claim of
  /// every older incarnation of the same source.
  Outcome<OpenSessionResult> OpenSession(const SourceDescriptor& descriptor, Incarnation incarnation);
  /// Marks a session no longer live. Its claims stay authoritative; only
  /// freshness changes.
  Outcome<void> CloseSession(SourceIncarnationKey key);
  [[nodiscard]] std::vector<SourceSessionView> Sessions() const;

  // -- evidence -----------------------------------------------------------

  /// Accepts one evidence record. The record is made durable before the call
  /// returns when the registry has a store.
  Outcome<IngestResult> Ingest(const Evidence& evidence);
  /// Accepts a batch. Every record is evaluated independently; one refusal
  /// does not abort the batch.
  std::vector<IngestResult> IngestBatch(std::span<const Evidence> evidence);

  // -- queries ------------------------------------------------------------

  Outcome<PortView> QueryPort(PortRef port, ValidationPolicy policy = ValidationPolicy::IncludeAll) const;
  Outcome<EndpointView> QueryEndpoint(EndpointId endpoint,
                                      bool include_ports = false,
                                      ValidationPolicy policy = ValidationPolicy::IncludeAll) const;
  Outcome<ObjectView> QueryObject(ObjectId object, const QueryOptions& options = {}) const;
  Outcome<TopologyView> QueryTopology(const QueryOptions& options = {}) const;
  Outcome<ObjectHistory> QueryHistory(ObjectId object, std::uint32_t max_events = 256) const;
  /// Every port in one attachment state, ordered by (endpoint, index).
  std::vector<PortView> QueryPortsByState(PortAttachmentState state,
                                          ValidationPolicy policy = ValidationPolicy::IncludeAll) const;
  /// Fenced sessions and objects, unvalidated ports, pending references and
  /// the bounded refusal audit.
  Outcome<InspectionView> Inspect(std::uint32_t max_entries = 1024) const;

  // -- export -------------------------------------------------------------

  /// The canonical immutable snapshot of the authoritative graph.
  Outcome<TopologySnapshot> Snapshot() const;
  /// The same graph with the metadata that must not change its identity.
  Outcome<SnapshotEnvelope> SnapshotWithMetadata() const;
  /// SHA-256 of the canonical snapshot encoding.
  Outcome<Digest256> GraphDigest() const;

  [[nodiscard]] RegistryStats Stats() const;

  /// Rewrites the log as a single state image. Bounded growth is enforced by
  /// the caller; the registry only performs the rewrite when asked. Fails on a
  /// registry with no store.
  Outcome<void> Compact();

  /// Stops accepting work. In-flight calls finish; later calls are refused
  /// with Closed. Idempotent.
  void Close();

 private:
  Registry();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cable_registry

#endif // CABLE_REGISTRY_REGISTRY_HPP
