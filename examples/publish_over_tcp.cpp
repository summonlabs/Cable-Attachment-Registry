// Cable Attachment Registry — publishing over the framed transport.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Starts a registry server on an ephemeral loopback port, publishes from a
// client, and reads the answer back. This is exactly what an out-of-process
// publisher does; only the process boundary is missing here.

#include <cstdio>
#include <memory>
#include <string>

#include "cable_registry/cable_attachment_registry.hpp"

using namespace cable_registry;

int main() {
  RegistryOptions registry_options;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(registry_options);
  if (!opened.has_value()) {
    return 1;
  }
  Registry& registry = *opened.value();

  ipc::ServerOptions server_options;
  server_options.port = 0;
  server_options.max_connections = 8;
  Outcome<std::unique_ptr<ipc::Server>> server = ipc::Server::Start(registry, server_options);
  if (!server.has_value()) {
    std::fprintf(stderr, "server start failed: %s\n", server.error().describe().c_str());
    return 1;
  }
  std::printf("registry listening on %s\n", server.value()->endpoint().c_str());

  ipc::ClientOptions client_options;
  client_options.host = "127.0.0.1";
  client_options.port = server.value()->port();
  Outcome<std::unique_ptr<ipc::Client>> connected = ipc::Client::Connect(client_options);
  if (!connected.has_value()) {
    std::fprintf(stderr, "connect failed: %s\n", connected.error().describe().c_str());
    return 1;
  }
  ipc::Client& client = *connected.value();
  std::printf("server identity %s, protocol %u\n", client.hello().identity.c_str(),
              client.hello().protocol_version);

  SourceDescriptor descriptor;
  descriptor.id = SourceId(Id128{0x0500000000000002ull, 1});
  descriptor.name = "remote-publisher";
  descriptor.authority.authority_class = AuthorityClass::Declared;
  descriptor.authority.rank = 1;

  const Outcome<OpenSessionResult> session = client.OpenSession(descriptor, Incarnation{7});
  if (!session.has_value() || session.value().disposition != OpenSessionResult::Disposition::Opened) {
    std::fprintf(stderr, "the remote session could not be opened\n");
    return 1;
  }

  const ObjectId cable = ObjectId(Id128{0x0B1EC70000000000ull, 42});
  ObjectDescriptor cable_descriptor;
  cable_descriptor.id = cable;
  cable_descriptor.kind = ObjectKind::Cable;
  cable_descriptor.physical_label = "C-42";
  cable_descriptor.sides.assign(1, ObjectSideDescriptor{ConnectorClass::Qsfp28,
                                                        MediaClass::DirectAttachCopper, LaneCount{4}});
  cable_descriptor.capability.connector = ConnectorClass::Qsfp28;
  cable_descriptor.capability.media = MediaClass::DirectAttachCopper;

  EndpointDescriptor endpoint;
  endpoint.id = EndpointId(Id128{0x0E0D000000000000ull, 7});
  endpoint.kind = EndpointKind::Switch;
  endpoint.name = "sw-07";
  endpoint.port_count = 4;
  endpoint.default_port_capability.accepted_connector = ConnectorClass::Qsfp28;

  Generation generation{};
  auto publish = [&](EvidencePayload payload) {
    generation.value += 1;
    Evidence evidence;
    evidence.header.id = EvidenceId(Id128{0x00E71DE000000000ull, generation.value});
    evidence.header.source = descriptor.id;
    evidence.header.incarnation = Incarnation{7};
    evidence.header.generation = generation;
    evidence.header.observed_at = now_timestamp();
    evidence.header.provenance = ProvenanceClass::OperatorEntry;
    evidence.payload = std::move(payload);
    const Outcome<IngestResult> result = client.Publish(evidence);
    if (!result.has_value()) {
      std::fprintf(stderr, "publish failed: %s\n", result.error().describe().c_str());
      return false;
    }
    std::printf("publish -> %s\n", to_string(result.value().disposition));
    return result.value().accepted();
  };

  if (!publish(RegisterObjectPayload{cable_descriptor}) ||
      !publish(RegisterEndpointPayload{endpoint})) {
    return 1;
  }
  AttachPayload attachment;
  attachment.subject = ObjectSideRef{cable, SideIndex{0}};
  attachment.port = PortRef{endpoint.id, PortIndex{1}};
  attachment.object_incarnation = ObjectIncarnation{1};
  if (!publish(attachment)) {
    return 1;
  }

  const Outcome<PortView> port = client.QueryPort(PortRef{endpoint.id, PortIndex{1}});
  if (!port.has_value()) {
    std::fprintf(stderr, "query failed: %s\n", port.error().describe().c_str());
    return 1;
  }
  std::printf("remote answer: port 1 is %s with %zu attachment(s)\n", to_string(port.value().state),
              port.value().edges.size());

  const Outcome<Digest256> digest = client.GraphDigest();
  if (digest.has_value()) {
    std::printf("remote graph digest %s\n", to_hex(digest.value()).c_str());
  }

  client.Close();
  server.value()->Stop();
  registry.Close();
  return 0;
}
