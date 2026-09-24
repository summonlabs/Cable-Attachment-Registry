// Cable Attachment Registry — shared builders for the registry test suites.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_TESTS_SUPPORT_REGISTRY_FIXTURE_HPP
#define CABLE_REGISTRY_TESTS_SUPPORT_REGISTRY_FIXTURE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "test_harness.hpp"

namespace crtest {

using namespace cable_registry;

/// A deterministic identity: readable, stable, and never nil.
inline ObjectId object_id(std::uint64_t lo) {
  return ObjectId::from_parts(0x0B1EC70000000000ull, lo);
}

inline EndpointId endpoint_id(std::uint64_t lo) {
  return EndpointId::from_parts(0x0E0D000000000000ull, lo);
}

inline SourceId source_id(std::uint64_t lo) {
  return SourceId::from_parts(0x0500000000000000ull | lo, 0x0000000000000001ull);
}

/// One publisher's monotonic evidence stream.
class Stream {
 public:
  Stream(SourceId id, Authority authority, ProvenanceClass provenance = ProvenanceClass::DiscoveryAgent)
      : id_(id), authority_(authority), provenance_(provenance) {}

  [[nodiscard]] SourceDescriptor descriptor(std::string name) const {
    SourceDescriptor descriptor;
    descriptor.id = id_;
    descriptor.name = std::move(name);
    descriptor.authority = authority_;
    descriptor.administrative_domain = "test";
    return descriptor;
  }

  [[nodiscard]] SourceId id() const noexcept { return id_; }
  [[nodiscard]] Incarnation incarnation() const noexcept { return incarnation_; }
  [[nodiscard]] Generation last_generation() const noexcept { return generation_; }
  void set_incarnation(Incarnation incarnation) noexcept { incarnation_ = incarnation; generation_ = Generation{}; }
  void skip(std::uint64_t generations) noexcept { generation_.value += generations; }

  /// Builds the next record of this stream.
  [[nodiscard]] Evidence make(EvidencePayload payload) {
    generation_.value += 1;
    Evidence evidence;
    evidence.header.id = EvidenceId::from_parts(0x00E71DE000000000ull | id_.value().hi, generation_.value);
    evidence.header.source = id_;
    evidence.header.incarnation = incarnation_;
    evidence.header.generation = generation_;
    evidence.header.observed_at = Timestamp{1'700'000'000'000'000'000ll +
                                            static_cast<std::int64_t>(generation_.value) * 1'000'000ll};
    evidence.header.provenance = provenance_;
    evidence.header.provenance_detail = "unit-test";
    evidence.payload = std::move(payload);
    return evidence;
  }

 private:
  SourceId id_{};
  Incarnation incarnation_{1};
  Generation generation_{};
  Authority authority_{};
  ProvenanceClass provenance_ = ProvenanceClass::Unknown;
};

inline ObjectDescriptor make_object(ObjectId id,
                                    std::string label,
                                    std::uint32_t sides = 2,
                                    ConnectorClass connector = ConnectorClass::Qsfp28,
                                    MediaClass media = MediaClass::DirectAttachCopper) {
  ObjectDescriptor descriptor;
  descriptor.id = id;
  descriptor.kind = ObjectKind::Cable;
  descriptor.physical_label = std::move(label);
  descriptor.serial_like = "SN-" + descriptor.physical_label;
  descriptor.administrative_location = "R01-U01-P01";
  descriptor.sides.assign(sides, ObjectSideDescriptor{connector, media, LaneCount{4}});
  descriptor.capability.media = media;
  descriptor.capability.connector = connector;
  descriptor.capability.lanes = LaneCount{4};
  descriptor.capability.capability_code = "400GBASE-CR4";
  return descriptor;
}

inline EndpointDescriptor make_endpoint(EndpointId id,
                                        std::string name,
                                        std::uint32_t ports = 8,
                                        std::uint32_t max_attachments = 1) {
  EndpointDescriptor descriptor;
  descriptor.id = id;
  descriptor.kind = EndpointKind::Switch;
  descriptor.name = std::move(name);
  descriptor.administrative_location = "R01-U01";
  descriptor.port_count = ports;
  descriptor.default_port_capability.max_simultaneous_attachments = max_attachments;
  descriptor.default_port_capability.accepted_connector = ConnectorClass::Qsfp28;
  descriptor.default_port_capability.accepted_media = MediaClass::DirectAttachCopper;
  return descriptor;
}

inline PortRef port_of(EndpointId endpoint, std::uint32_t index) {
  return PortRef{endpoint, PortIndex{index}};
}

inline AttachPayload attach(ObjectId object,
                            std::uint32_t side,
                            EndpointId endpoint,
                            std::uint32_t port,
                            ObjectIncarnation incarnation = ObjectIncarnation{1},
                            std::uint32_t slot = 0) {
  AttachPayload payload;
  payload.subject = ObjectSideRef{object, SideIndex{side}};
  payload.port = port_of(endpoint, port);
  payload.object_incarnation = incarnation;
  payload.slot = AttachmentSlot{slot};
  return payload;
}

inline DetachPayload detach(ObjectId object,
                            std::uint32_t side,
                            bool from_port,
                            EndpointId endpoint,
                            std::uint32_t port,
                            ObjectIncarnation incarnation = ObjectIncarnation{1},
                            std::uint32_t slot = 0) {
  DetachPayload payload;
  payload.subject = ObjectSideRef{object, SideIndex{side}};
  payload.object_incarnation = incarnation;
  payload.has_from_port = from_port;
  payload.from_port = port_of(endpoint, port);
  payload.slot = AttachmentSlot{slot};
  return payload;
}

inline MovePayload move(ObjectId object,
                        std::uint32_t side,
                        EndpointId from_endpoint,
                        std::uint32_t from_port,
                        EndpointId to_endpoint,
                        std::uint32_t to_port,
                        ObjectIncarnation incarnation = ObjectIncarnation{1}) {
  MovePayload payload;
  payload.subject = ObjectSideRef{object, SideIndex{side}};
  payload.object_incarnation = incarnation;
  payload.from = port_of(from_endpoint, from_port);
  payload.to = port_of(to_endpoint, to_port);
  return payload;
}

inline SetLifecyclePayload lifecycle(ObjectId object, LifecycleAction action, std::string reason = {}) {
  SetLifecyclePayload payload;
  payload.object = object;
  payload.object_incarnation = ObjectIncarnation{1};
  payload.action = action;
  payload.reason = std::move(reason);
  return payload;
}

/// An in-memory registry plus helpers that fail the running case on an
/// unexpected outcome.
class Fixture {
 public:
  explicit Fixture(Limits limits = Limits{}, std::uint32_t history_per_key = 8) {
    RegistryOptions options;
    options.limits = limits;
    options.history_per_key = history_per_key;
    Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
    if (!opened.has_value()) {
      ::crtest::fail(__FILE__, __LINE__, "registry open failed: " + opened.error().describe());
    }
    registry = std::move(opened).value();
  }

  Registry& operator*() const { return *registry; }
  Registry* operator->() const { return registry.get(); }

  /// Opens a session and fails the case when it is not accepted.
  OpenSessionResult open(const Stream& stream, const std::string& name) {
    Outcome<OpenSessionResult> result = registry->OpenSession(stream.descriptor(name), stream.incarnation());
    if (!result.has_value()) {
      ::crtest::fail(__FILE__, __LINE__, "open session failed: " + result.error().describe());
    }
    return result.value();
  }

  /// Ingests and fails the case unless the record was stored.
  IngestResult ingest(const Evidence& evidence) {
    Outcome<IngestResult> result = registry->Ingest(evidence);
    if (!result.has_value()) {
      ::crtest::fail(__FILE__, __LINE__, "ingest failed: " + result.error().describe());
    }
    if (!result.value().accepted()) {
      ::crtest::fail(__FILE__, __LINE__,
                     std::string("ingest refused: ") + to_string(result.value().disposition) + ": " +
                         result.value().detail);
    }
    return result.value();
  }

  /// Ingests and returns the disposition whatever it is.
  IngestResult try_ingest(const Evidence& evidence) {
    Outcome<IngestResult> result = registry->Ingest(evidence);
    if (!result.has_value()) {
      ::crtest::fail(__FILE__, __LINE__, "ingest failed: " + result.error().describe());
    }
    return result.value();
  }

  PortView port(EndpointId endpoint, std::uint32_t index,
                ValidationPolicy policy = ValidationPolicy::IncludeAll) {
    Outcome<PortView> view = registry->QueryPort(port_of(endpoint, index), policy);
    if (!view.has_value()) {
      ::crtest::fail(__FILE__, __LINE__, "query port failed: " + view.error().describe());
    }
    return view.value();
  }

  ObjectView object(ObjectId id, QueryOptions options = {}) {
    Outcome<ObjectView> view = registry->QueryObject(id, options);
    if (!view.has_value()) {
      ::crtest::fail(__FILE__, __LINE__, "query object failed: " + view.error().describe());
    }
    return view.value();
  }

  TopologySnapshot snapshot() {
    Outcome<TopologySnapshot> value = registry->Snapshot();
    if (!value.has_value()) {
      ::crtest::fail(__FILE__, __LINE__, "snapshot failed: " + value.error().describe());
    }
    return value.value();
  }

 private:
  std::unique_ptr<Registry> registry;
};

} // namespace crtest

#endif // CABLE_REGISTRY_TESTS_SUPPORT_REGISTRY_FIXTURE_HPP
