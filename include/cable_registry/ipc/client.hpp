// Cable Attachment Registry — publisher/query client.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_IPC_CLIENT_HPP
#define CABLE_REGISTRY_IPC_CLIENT_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "cable_registry/authority.hpp"
#include "cable_registry/error.hpp"
#include "cable_registry/evidence.hpp"
#include "cable_registry/export.hpp"
#include "cable_registry/graph.hpp"
#include "cable_registry/ipc/protocol.hpp"
#include "cable_registry/limits.hpp"
#include "cable_registry/registry.hpp"
#include "cable_registry/snapshot.hpp"

namespace cable_registry::ipc {

struct ClientOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  Limits limits{};
};

/// A connection to a registry.
///
/// One request is in flight at a time per client: the framing carries a
/// request identifier, and the client refuses a reply whose identifier does
/// not match the request it sent.
class CABLE_REGISTRY_API Client {
 public:
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  ~Client();

  static Outcome<std::unique_ptr<Client>> Connect(const ClientOptions& options);

  [[nodiscard]] const Hello& hello() const noexcept;

  Outcome<OpenSessionResult> OpenSession(const SourceDescriptor& descriptor, Incarnation incarnation);
  Outcome<void> CloseSession(SourceIncarnationKey key);

  Outcome<IngestResult> Publish(const Evidence& evidence);
  std::vector<IngestResult> PublishBatch(std::span<const Evidence> evidence);

  Outcome<PortView> QueryPort(PortRef port, ValidationPolicy policy = ValidationPolicy::IncludeAll);
  Outcome<EndpointView> QueryEndpoint(EndpointId endpoint,
                                      bool include_ports = false,
                                      ValidationPolicy policy = ValidationPolicy::IncludeAll);
  Outcome<ObjectView> QueryObject(ObjectId object, const QueryOptions& options = {});
  Outcome<ObjectHistory> QueryHistory(ObjectId object, std::uint32_t max_events = 256);
  Outcome<InspectionView> Inspect(std::uint32_t max_entries = 1024);
  Outcome<TopologySnapshot> Snapshot();
  Outcome<Digest256> GraphDigest();
  Outcome<RegistryStats> Stats();
  Outcome<void> Shutdown();

  void Close();

 private:
  Client();
  Outcome<Frame> request(MessageType type, std::span<const std::byte> payload);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cable_registry::ipc

#endif // CABLE_REGISTRY_IPC_CLIENT_HPP
