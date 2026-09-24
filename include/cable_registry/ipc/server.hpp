// Cable Attachment Registry — loopback publisher/registry server.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_IPC_SERVER_HPP
#define CABLE_REGISTRY_IPC_SERVER_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "cable_registry/error.hpp"
#include "cable_registry/export.hpp"
#include "cable_registry/limits.hpp"
#include "cable_registry/registry.hpp"

namespace cable_registry::ipc {

/// Server policy. Every bound is enforced per connection and per thread.
struct ServerOptions {
  /// Interface to bind. The default keeps the registry on the loopback
  /// interface; binding anywhere else has to be asked for explicitly.
  std::string bind_address = "127.0.0.1";
  /// TCP port. Zero asks the operating system for an ephemeral port, which
  /// port() then reports.
  std::uint16_t port = 0;
  /// Connections served at once. A publisher holds its connection for as long
  /// as it needs it, so each connection is served by its own thread and this
  /// bound is what keeps the server's thread count finite. A connection beyond
  /// the bound is refused rather than queued, because queueing it would block
  /// a publisher that did nothing wrong.
  std::uint32_t max_connections = 32;
  std::uint32_t listen_backlog = 32;
  Limits limits{};
  /// Whether a client may ask the registry to shut down over the wire.
  bool allow_shutdown = true;
};

/// Counters for the server.
struct ServerStats {
  std::uint64_t connections_accepted = 0;
  std::uint64_t connections_rejected = 0;
  std::uint64_t requests_served = 0;
  std::uint64_t protocol_failures = 0;
  std::uint64_t frames_rejected = 0;
};

/// Serves one registry over a framed TCP transport.
///
/// Stop() stops accepting, wakes every worker through its own socket, joins
/// them outside any lock, and only then returns. There is no timeout and no
/// forced termination anywhere on this path.
class CABLE_REGISTRY_API Server {
 public:
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;
  ~Server();

  static Outcome<std::unique_ptr<Server>> Start(Registry& registry, const ServerOptions& options);

  /// The bound port, valid until Stop().
  [[nodiscard]] std::uint16_t port() const noexcept;
  /// "address:port" as bound.
  [[nodiscard]] std::string endpoint() const;
  [[nodiscard]] ServerStats stats() const;

  /// Blocks until a client asked the registry to shut down, or until Stop()
  /// is called. Returns true when the request came from a client. The owner of
  /// the server calls Stop() itself; a connection handler never stops the
  /// server it is running on.
  bool wait_for_shutdown_request();

  /// Stops serving. Idempotent and safe to call from any thread other than a
  /// connection handler.
  void Stop();

 private:
  Server();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cable_registry::ipc

#endif // CABLE_REGISTRY_IPC_SERVER_HPP
