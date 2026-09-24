// Cable Attachment Registry — embedding the runtime in a process.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program registers a small patch, attaches both ends of a cable, moves
// one end, and prints the authoritative answer plus the canonical digest. It
// is the shortest complete use of the library.

#include <cstdio>
#include <memory>
#include <string>

#include "cable_registry/cable_attachment_registry.hpp"

using namespace cable_registry;

namespace {

Id128 id_from(std::uint64_t value) {
  return Id128{0x0B1EC70000000000ull, value};
}

} // namespace

int main() {
  RegistryOptions options;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
  if (!opened.has_value()) {
    std::fprintf(stderr, "open failed: %s\n", opened.error().describe().c_str());
    return 1;
  }
  Registry& registry = *opened.value();

  const SourceId source = SourceId(Id128{0x0500000000000001ull, 1});
  SourceDescriptor descriptor;
  descriptor.id = source;
  descriptor.name = "discovery-agent";
  descriptor.authority.authority_class = AuthorityClass::Observed;
  descriptor.authority.rank = 10;

  const Outcome<OpenSessionResult> session = registry.OpenSession(descriptor, Incarnation{1});
  if (!session.has_value() || session.value().disposition != OpenSessionResult::Disposition::Opened) {
    std::fprintf(stderr, "the publisher session could not be opened\n");
    return 1;
  }

  Generation generation{};
  auto publish = [&](EvidencePayload payload) {
    generation.value += 1;
    Evidence evidence;
    evidence.header.id = EvidenceId(Id128{0x00E71DE000000000ull, generation.value});
    evidence.header.source = source;
    evidence.header.incarnation = Incarnation{1};
    evidence.header.generation = generation;
    evidence.header.observed_at = now_timestamp();
    evidence.header.provenance = ProvenanceClass::DiscoveryAgent;
    evidence.payload = std::move(payload);
    const Outcome<IngestResult> result = registry.Ingest(evidence);
    if (!result.has_value()) {
      std::fprintf(stderr, "ingest failed: %s\n", result.error().describe().c_str());
      return false;
    }
    if (!result.value().accepted()) {
      std::fprintf(stderr, "refused: %s: %s\n", to_string(result.value().disposition),
                   result.value().detail.c_str());
      return false;
    }
    return true;
  };

  const ObjectId cable = ObjectId(id_from(1));
  const EndpointId patch_panel = EndpointId(id_from(2));

  ObjectDescriptor cable_descriptor;
  cable_descriptor.id = cable;
  cable_descriptor.kind = ObjectKind::PatchCord;
  cable_descriptor.physical_label = "C-17";
  cable_descriptor.serial_like = "SN-0001";
  cable_descriptor.administrative_location = "R04-U12-P3";
  cable_descriptor.sides.assign(2, ObjectSideDescriptor{ConnectorClass::Lc, MediaClass::SinglemodeFiber,
                                                        LaneCount{1}});
  cable_descriptor.capability.connector = ConnectorClass::Lc;
  cable_descriptor.capability.media = MediaClass::SinglemodeFiber;

  EndpointDescriptor panel_descriptor;
  panel_descriptor.id = patch_panel;
  panel_descriptor.kind = EndpointKind::PatchPanel;
  panel_descriptor.name = "pp-04";
  panel_descriptor.administrative_location = "R04-U12";
  panel_descriptor.port_count = 24;

  if (!publish(RegisterObjectPayload{cable_descriptor}) ||
      !publish(RegisterEndpointPayload{panel_descriptor})) {
    return 1;
  }

  AttachPayload first_attachment;
  first_attachment.subject = ObjectSideRef{cable, SideIndex{0}};
  first_attachment.port = PortRef{patch_panel, PortIndex{3}};
  first_attachment.object_incarnation = ObjectIncarnation{1};

  AttachPayload second_attachment;
  second_attachment.subject = ObjectSideRef{cable, SideIndex{1}};
  second_attachment.port = PortRef{patch_panel, PortIndex{4}};
  second_attachment.object_incarnation = ObjectIncarnation{1};

  if (!publish(first_attachment) || !publish(second_attachment)) {
    return 1;
  }

  MovePayload move;
  move.subject = ObjectSideRef{cable, SideIndex{0}};
  move.from = PortRef{patch_panel, PortIndex{3}};
  move.to = PortRef{patch_panel, PortIndex{9}};
  move.object_incarnation = ObjectIncarnation{1};
  if (!publish(move)) {
    return 1;
  }

  for (const std::uint32_t index : {3u, 4u, 9u}) {
    const Outcome<PortView> port = registry.QueryPort(PortRef{patch_panel, PortIndex{index}});
    if (!port.has_value()) {
      std::fprintf(stderr, "query failed: %s\n", port.error().describe().c_str());
      return 1;
    }
    std::printf("port %u is %s\n", index, to_string(port.value().state));
    if (port.value().state == PortAttachmentState::Attached) {
      const AttachmentEdge& edge = port.value().edges.front();
      std::printf("  held by cable %s side %u, asserted by %zu claim(s) at %s authority\n",
                  edge.subject.object.to_hex().c_str(),
                  edge.subject.side.value,
                  edge.claims.size(),
                  to_string(edge.authority.authority_class));
    }
  }

  const Outcome<ObjectView> object = registry.QueryObject(cable);
  if (!object.has_value()) {
    return 1;
  }
  std::printf("cable %s is %s\n", object.value().physical_label.c_str(),
              to_string(object.value().lifecycle));

  const Outcome<TopologySnapshot> snapshot = registry.Snapshot();
  if (!snapshot.has_value()) {
    return 1;
  }
  std::printf("graph digest %s over %llu port(s) and %llu object(s)\n",
              to_hex(snapshot.value().graph_digest()).c_str(),
              static_cast<unsigned long long>(snapshot.value().summary.ports_attached +
                                              snapshot.value().summary.ports_empty),
              static_cast<unsigned long long>(snapshot.value().summary.objects_total));

  registry.Close();
  return 0;
}
