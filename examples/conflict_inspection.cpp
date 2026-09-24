// Cable Attachment Registry — preserving and inspecting a conflict.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two sources of equal authority disagree about what is plugged into one port.
// The registry refuses to choose: both claims are reported, and the port is
// CONFLICTING until a source with more authority, or the same sources with
// newer evidence, settle it.

#include <cstdio>
#include <memory>
#include <string>

#include "cable_registry/cable_attachment_registry.hpp"

using namespace cable_registry;

namespace {

struct Publisher {
  Registry* registry = nullptr;
  SourceId id{};
  Incarnation incarnation{1};
  Generation generation{};

  bool open(const std::string& name, AuthorityClass authority_class) {
    SourceDescriptor descriptor;
    descriptor.id = id;
    descriptor.name = name;
    descriptor.authority.authority_class = authority_class;
    descriptor.authority.rank = 1;
    const Outcome<OpenSessionResult> result = registry->OpenSession(descriptor, incarnation);
    if (!result.has_value()) {
      std::fprintf(stderr, "open failed: %s\n", result.error().describe().c_str());
      return false;
    }
    if (result.value().disposition == OpenSessionResult::Disposition::RefusedDescriptorConflict ||
        result.value().disposition == OpenSessionResult::Disposition::RefusedFenced ||
        result.value().disposition == OpenSessionResult::Disposition::RefusedInvalid) {
      std::fprintf(stderr, "the session was refused: %s\n", to_string(result.value().disposition));
      return false;
    }
    return true;
  }

  bool publish(EvidencePayload payload) {
    generation.value += 1;
    Evidence evidence;
    evidence.header.id = EvidenceId(Id128{id.value().hi ^ 0x00E71DE000000000ull, generation.value});
    evidence.header.source = id;
    evidence.header.incarnation = incarnation;
    evidence.header.generation = generation;
    evidence.header.observed_at = now_timestamp();
    evidence.header.provenance = ProvenanceClass::Reconciliation;
    evidence.payload = std::move(payload);
    const Outcome<IngestResult> result = registry->Ingest(evidence);
    if (!result.has_value()) {
      std::fprintf(stderr, "publish failed: %s\n", result.error().describe().c_str());
      return false;
    }
    if (!result.value().accepted()) {
      std::fprintf(stderr, "refused: %s: %s\n", to_string(result.value().disposition),
                   result.value().detail.c_str());
      return false;
    }
    return true;
  }
};

} // namespace

int main() {
  RegistryOptions options;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
  if (!opened.has_value()) {
    std::fprintf(stderr, "open failed: %s\n", opened.error().describe().c_str());
    return 1;
  }
  Registry& registry = *opened.value();

  Publisher left{&registry, SourceId(Id128{0x0500000000000010ull, 1})};
  Publisher right{&registry, SourceId(Id128{0x0500000000000011ull, 1})};
  if (!left.open("runbook-left", AuthorityClass::Observed) ||
      !right.open("runbook-right", AuthorityClass::Observed)) {
    return 1;
  }

  const ObjectId cable_a = ObjectId(Id128{0x0B1EC70000000000ull, 100});
  const ObjectId cable_b = ObjectId(Id128{0x0B1EC70000000000ull, 101});
  const EndpointId switch_id = EndpointId(Id128{0x0E0D000000000000ull, 200});

  EndpointDescriptor endpoint;
  endpoint.id = switch_id;
  endpoint.kind = EndpointKind::Switch;
  endpoint.name = "sw-200";
  endpoint.port_count = 2;
  if (!left.publish(RegisterEndpointPayload{endpoint})) {
    std::fprintf(stderr, "the endpoint registration was refused\n");
    return 1;
  }
  for (const ObjectId object : {cable_a, cable_b}) {
    ObjectDescriptor cable;
    cable.id = object;
    cable.kind = ObjectKind::Cable;
    cable.physical_label = "X-" + object.to_hex().substr(28);
    cable.sides.assign(1, ObjectSideDescriptor{ConnectorClass::Qsfp28, MediaClass::DirectAttachCopper,
                                               LaneCount{4}});
    cable.capability.connector = ConnectorClass::Qsfp28;
    cable.capability.media = MediaClass::DirectAttachCopper;
    if (!left.publish(RegisterObjectPayload{cable})) {
      std::fprintf(stderr, "an object registration was refused\n");
      return 1;
    }
  }

  // Object incarnations start at one: zero means "not stated" and is refused.
  AttachPayload first;
  first.subject = ObjectSideRef{cable_a, SideIndex{0}};
  first.port = PortRef{switch_id, PortIndex{0}};
  first.object_incarnation = ObjectIncarnation{1};
  AttachPayload second;
  second.subject = ObjectSideRef{cable_b, SideIndex{0}};
  second.port = PortRef{switch_id, PortIndex{0}};
  second.object_incarnation = ObjectIncarnation{1};

  if (!left.publish(first) || !right.publish(second)) {
    std::fprintf(stderr, "an attachment was refused\n");
    return 1;
  }

  const Outcome<PortView> conflicted = registry.QueryPort(PortRef{switch_id, PortIndex{0}});
  if (!conflicted.has_value()) {
    std::fprintf(stderr, "query failed: %s\n", conflicted.error().describe().c_str());
    return 1;
  }
  std::printf("port 0 is %s with %zu candidate(s)\n", to_string(conflicted.value().state),
              conflicted.value().edges.size());
  for (const ConflictNote& note : conflicted.value().conflicts) {
    std::printf("  conflict %s: %s\n", to_string(note.reason), note.detail.c_str());
    for (const ClaimProvenance& claim : note.claims) {
      std::printf("    %s generation %llu at %s authority\n", claim.source.to_hex().c_str(),
                  static_cast<unsigned long long>(claim.generation.value),
                  to_string(claim.authority.authority_class));
    }
  }

  // Nothing is silently chosen, and the canonical snapshot carries the conflict
  // as data rather than resolving it.
  const Outcome<TopologySnapshot> snapshot = registry.Snapshot();
  if (!snapshot.has_value()) {
    std::fprintf(stderr, "snapshot failed: %s\n", snapshot.error().describe().c_str());
    return 1;
  }
  for (const SnapshotPort& port : snapshot.value().ports) {
    if (port.port.index.value == 0) {
      std::printf("snapshot records %s with %zu attachment(s) and %zu conflict(s)\n",
                  to_string(port.state), port.attachments.size(), port.conflicts.size());
    }
  }
  std::printf("graph digest %s\n", to_hex(snapshot.value().graph_digest()).c_str());

  registry.Close();
  return 0;
}
