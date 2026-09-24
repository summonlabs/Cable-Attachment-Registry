// Cable Attachment Registry — downstream consumer.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is built by an independent CMake project against an installed
// package: it never sees the registry's source tree or build tree. It uses the
// public headers, links the exported target, and exercises the runtime end to
// end so that a broken install, a missing include or an unstaged dependency
// fails the build or the run.

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <cable_registry/cable_attachment_registry.hpp>

using namespace cable_registry;

int main() {
  if (std::string(version_string()) != "1.0.0") {
    std::fprintf(stderr, "unexpected library version %s\n", version_string().c_str());
    return 1;
  }

  RegistryOptions options;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
  if (!opened.has_value()) {
    std::fprintf(stderr, "open failed: %s\n", opened.error().describe().c_str());
    return 1;
  }
  Registry& registry = *opened.value();

  const SourceId source = SourceId(Id128{0x0500000000000003ull, 1});
  SourceDescriptor descriptor;
  descriptor.id = source;
  descriptor.name = "consumer";
  descriptor.authority.authority_class = AuthorityClass::Observed;
  if (!registry.OpenSession(descriptor, Incarnation{1}).has_value()) {
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
    evidence.header.observed_at = Timestamp{1'700'000'000'000'000'000ll +
                                            static_cast<std::int64_t>(generation.value)};
    evidence.header.provenance = ProvenanceClass::Synthetic;
    evidence.payload = std::move(payload);
    const Outcome<IngestResult> result = registry.Ingest(evidence);
    return result.has_value() && result.value().accepted();
  };

  const ObjectId cable = ObjectId(Id128{0x0B1EC70000000000ull, 7});
  const EndpointId switch_id = EndpointId(Id128{0x0E0D000000000000ull, 7});

  ObjectDescriptor cable_descriptor;
  cable_descriptor.id = cable;
  cable_descriptor.kind = ObjectKind::Cable;
  cable_descriptor.physical_label = "CONSUMER-1";
  cable_descriptor.sides.assign(2, ObjectSideDescriptor{ConnectorClass::Qsfp28,
                                                        MediaClass::DirectAttachCopper, LaneCount{4}});
  cable_descriptor.capability.capability_code = "400GBASE-CR4";

  EndpointDescriptor endpoint;
  endpoint.id = switch_id;
  endpoint.kind = EndpointKind::Switch;
  endpoint.name = "consumer-sw";
  endpoint.port_count = 4;

  if (!publish(RegisterObjectPayload{cable_descriptor}) ||
      !publish(RegisterEndpointPayload{endpoint})) {
    return 1;
  }

  AttachPayload attachment;
  attachment.subject = ObjectSideRef{cable, SideIndex{0}};
  attachment.port = PortRef{switch_id, PortIndex{2}};
  attachment.object_incarnation = ObjectIncarnation{1};
  if (!publish(attachment)) {
    return 1;
  }

  const Outcome<PortView> port = registry.QueryPort(PortRef{switch_id, PortIndex{2}});
  if (!port.has_value() || port.value().state != PortAttachmentState::Attached) {
    std::fprintf(stderr, "the consumer could not read back its own attachment\n");
    return 1;
  }

  const Outcome<TopologySnapshot> snapshot = registry.Snapshot();
  if (!snapshot.has_value()) {
    return 1;
  }
  const std::vector<std::byte> encoded = snapshot.value().encode();
  const Outcome<TopologySnapshot> decoded = decode_snapshot(encoded, Limits{});
  if (!decoded.has_value() || decoded.value().graph_digest() != snapshot.value().graph_digest()) {
    std::fprintf(stderr, "the canonical image did not round trip\n");
    return 1;
  }

  // The transport is part of the installed package too.
  ipc::ServerOptions server_options;
  server_options.port = 0;
  server_options.max_connections = 4;
  Outcome<std::unique_ptr<ipc::Server>> server = ipc::Server::Start(registry, server_options);
  if (!server.has_value()) {
    std::fprintf(stderr, "server start failed: %s\n", server.error().describe().c_str());
    return 1;
  }
  ipc::ClientOptions client_options;
  client_options.host = "127.0.0.1";
  client_options.port = server.value()->port();
  Outcome<std::unique_ptr<ipc::Client>> client = ipc::Client::Connect(client_options);
  if (!client.has_value()) {
    std::fprintf(stderr, "connect failed: %s\n", client.error().describe().c_str());
    return 1;
  }
  const Outcome<PortView> remote = client.value()->QueryPort(PortRef{switch_id, PortIndex{2}});
  if (!remote.has_value() || remote.value().state != PortAttachmentState::Attached) {
    std::fprintf(stderr, "the consumer could not query over the transport\n");
    return 1;
  }
  client.value()->Close();
  server.value()->Stop();

  std::printf("consumer ok: %s on port 2, graph digest %s\n",
              to_string(remote.value().state),
              to_hex(snapshot.value().graph_digest()).c_str());
  registry.Close();
  return 0;
}
