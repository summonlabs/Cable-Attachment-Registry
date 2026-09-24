// Cable Attachment Registry — loopback publisher/registry server.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The protocol keeps a connection open for as long as a publisher needs it, so
// one connection is served by one thread rather than by a fixed pool: a pool of
// N workers can never serve more than N persistent connections, and queueing
// the rest would block publishers that did nothing wrong. The number of
// concurrently served connections is bounded, and a connection beyond the
// bound is refused rather than queued.
//
// Shutdown is cooperative, bounded and complete. Stop() sets a stop flag,
// wakes the acceptor through a dedicated wakeup socket (a blocked accept()
// cannot be cancelled portably any other way), joins the acceptor, and waits
// for the live connection count to reach zero. Connection threads never block
// indefinitely inside the operating system: they wait for readiness in short
// slices and check the stop flag between slices, so nothing has to be
// cancelled from another thread, which Windows does not support for a blocked
// receive. No lock is held while waiting for a thread, and no connection
// outlives the server.

#include "cable_registry/ipc/server.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "cable_registry/codec.hpp"
#include "cable_registry/evidence.hpp"
#include "cable_registry/ipc/protocol.hpp"
#include "cable_registry/serialization.hpp"
#include "cable_registry/version.hpp"
#include "ipc/messages.hpp"
#include "ipc/socket.hpp"

namespace cable_registry::ipc {
namespace {

/// Sends one frame. A failure is ignored here: the connection is already gone
/// and the thread is about to close it.
void send_frame(detail::Socket& connection,
                MessageType type,
                std::uint64_t request_id,
                std::span<const std::byte> payload,
                std::uint32_t max_payload,
                const std::atomic<bool>& stop,
                int poll_ms) {
  const std::vector<std::byte> frame = encode_frame(type, request_id, payload, max_payload);
  if (frame.empty()) {
    return;
  }
  const Outcome<void> sent = connection.send_all(frame, stop, poll_ms);
  (void)sent;
}

} // namespace

struct Server::Impl {
  Registry* registry = nullptr;
  ServerOptions options;
  detail::Socket listener;
  detail::Socket wakeup_reader;
  detail::Socket wakeup_writer;
  std::uint16_t bound_port = 0;

  /// I/O multiplexing interval used by the connection threads. It bounds how
  /// long a stop request waits for a thread that is idle on the wire; it is
  /// not a session deadline and never cuts a transfer short.
  static constexpr int kPollMs = 25;

  std::mutex mutex;
  std::condition_variable live_cv;
  std::condition_variable shutdown_requested_cv;
  std::set<detail::Socket*> active;
  std::size_t live_connections = 0;
  std::thread acceptor;
  std::atomic<bool> stop_flag{false};
  bool stopping = false;
  bool shutdown_requested = false;
  ServerStats stats;

  MessageType dispatch(const Frame& request, std::vector<std::byte>& response) {
    Registry& registry_ref = *registry;
    const Limits& limits = options.limits;

    auto fail = [&](const Error& error) {
      Encoder encoder;
      encode_failure(encoder, error, limits);
      response = encoder.take();
      return MessageType::Failure;
    };

    switch (request.type) {
      case MessageType::Hello: {
        Hello hello;
        hello.protocol_version = kProtocolVersion;
        hello.library_version = version_number();
        hello.registry = registry_ref.id();
        hello.max_payload_bytes = limits.max_frame_payload_bytes;
        hello.shutdown_allowed = options.allow_shutdown;
        hello.identity = std::string("cable-registry/") + version_string();
        response = encode_hello(hello, limits);
        return MessageType::HelloAck;
      }
      case MessageType::OpenSession: {
        SourceDescriptor descriptor;
        Incarnation incarnation;
        if (!decode_open_session_request(request.payload, limits, descriptor, incarnation)) {
          return fail(Error(ErrorCode::ProtocolViolation, "the open-session request is malformed"));
        }
        const Outcome<OpenSessionResult> result = registry_ref.OpenSession(descriptor, incarnation);
        if (!result.has_value()) {
          return fail(result.error());
        }
        Encoder encoder;
        encode_open_session_result(encoder, result.value(), limits);
        response = encoder.take();
        return MessageType::OpenSessionAck;
      }
      case MessageType::CloseSession: {
        SourceIncarnationKey key;
        if (!decode_close_session_request(request.payload, key)) {
          return fail(Error(ErrorCode::ProtocolViolation, "the close-session request is malformed"));
        }
        const Outcome<void> closed = registry_ref.CloseSession(key);
        if (!closed.has_value()) {
          return fail(closed.error());
        }
        response.clear();
        return MessageType::CloseSessionAck;
      }
      case MessageType::Publish: {
        const Outcome<Evidence> evidence = decode_evidence(request.payload, limits);
        if (!evidence.has_value()) {
          return fail(evidence.error());
        }
        const Outcome<IngestResult> result = registry_ref.Ingest(evidence.value());
        if (!result.has_value()) {
          return fail(result.error());
        }
        Encoder encoder;
        encode_ingest_result(encoder, result.value(), limits);
        response = encoder.take();
        return MessageType::PublishAck;
      }
      case MessageType::PublishBatch: {
        std::vector<Evidence> batch;
        if (!decode_publish_batch_request(request.payload, limits, batch)) {
          return fail(Error(ErrorCode::ProtocolViolation, "the publish-batch request is malformed"));
        }
        const std::vector<IngestResult> results = registry_ref.IngestBatch(batch);
        response = encode_ingest_results(results, limits);
        return MessageType::PublishBatchAck;
      }
      case MessageType::QueryPort: {
        PortRef port;
        ValidationPolicy policy = ValidationPolicy::IncludeAll;
        if (!decode_port_query(request.payload, port, policy)) {
          return fail(Error(ErrorCode::ProtocolViolation, "the query-port request is malformed"));
        }
        const Outcome<PortView> view = registry_ref.QueryPort(port, policy);
        if (!view.has_value()) {
          return fail(view.error());
        }
        Encoder encoder;
        encode_port_view(encoder, view.value(), limits);
        response = encoder.take();
        return MessageType::QueryPortAck;
      }
      case MessageType::QueryEndpoint: {
        EndpointId endpoint;
        bool include_ports = false;
        ValidationPolicy policy = ValidationPolicy::IncludeAll;
        if (!decode_endpoint_query(request.payload, endpoint, include_ports, policy)) {
          return fail(Error(ErrorCode::ProtocolViolation, "the query-endpoint request is malformed"));
        }
        const Outcome<EndpointView> view = registry_ref.QueryEndpoint(endpoint, include_ports, policy);
        if (!view.has_value()) {
          return fail(view.error());
        }
        Encoder encoder;
        encode_endpoint_view(encoder, view.value(), limits);
        response = encoder.take();
        return MessageType::QueryEndpointAck;
      }
      case MessageType::QueryObject: {
        ObjectId object;
        QueryOptions query_options;
        if (!decode_object_query(request.payload, object, query_options)) {
          return fail(Error(ErrorCode::ProtocolViolation, "the query-object request is malformed"));
        }
        const Outcome<ObjectView> view = registry_ref.QueryObject(object, query_options);
        if (!view.has_value()) {
          return fail(view.error());
        }
        Encoder encoder;
        encode_object_view(encoder, view.value(), limits);
        response = encoder.take();
        return MessageType::QueryObjectAck;
      }
      case MessageType::QueryHistory: {
        ObjectId object;
        std::uint32_t max_events = 0;
        if (!decode_history_query(request.payload, object, max_events)) {
          return fail(Error(ErrorCode::ProtocolViolation, "the query-history request is malformed"));
        }
        const Outcome<ObjectHistory> history = registry_ref.QueryHistory(object, max_events);
        if (!history.has_value()) {
          return fail(history.error());
        }
        Encoder encoder;
        encode_object_history(encoder, history.value(), limits);
        response = encoder.take();
        return MessageType::QueryHistoryAck;
      }
      case MessageType::Inspect: {
        std::uint32_t max_entries = 0;
        if (!decode_inspect_query(request.payload, max_entries)) {
          return fail(Error(ErrorCode::ProtocolViolation, "the inspect request is malformed"));
        }
        const Outcome<InspectionView> view = registry_ref.Inspect(max_entries);
        if (!view.has_value()) {
          return fail(view.error());
        }
        Encoder encoder;
        encode_inspection_view(encoder, view.value(), limits);
        response = encoder.take();
        return MessageType::InspectAck;
      }
      case MessageType::Snapshot: {
        const Outcome<TopologySnapshot> snapshot = registry_ref.Snapshot();
        if (!snapshot.has_value()) {
          return fail(snapshot.error());
        }
        Encoder encoder;
        encode_topology_snapshot(encoder, snapshot.value(), limits);
        response = encoder.take();
        if (response.size() + kFrameHeaderBytes > limits.max_frame_payload_bytes) {
          return fail(Error(ErrorCode::CapacityExceeded,
                            "the snapshot does not fit in one frame; query a smaller part of the graph or "
                            "raise the payload limit"));
        }
        return MessageType::SnapshotAck;
      }
      case MessageType::Digest: {
        const Outcome<Digest256> digest = registry_ref.GraphDigest();
        if (!digest.has_value()) {
          return fail(digest.error());
        }
        Encoder encoder;
        encode_digest(encoder, digest.value());
        response = encoder.take();
        return MessageType::DigestAck;
      }
      case MessageType::Stats: {
        Encoder encoder;
        encode_registry_stats(encoder, registry_ref.Stats(), limits);
        response = encoder.take();
        return MessageType::StatsAck;
      }
      case MessageType::Shutdown: {
        if (!options.allow_shutdown) {
          return fail(Error(ErrorCode::PermissionDenied, "remote shutdown is disabled on this server"));
        }
        response.clear();
        return MessageType::ShutdownAck;
      }
      default:
        return fail(Error(ErrorCode::ProtocolViolation,
                          std::string("message type ") + to_string(request.type) + " is not a request"));
    }
  }

  void serve_connection(detail::Socket& connection) {
    const Limits& limits = options.limits;
    std::array<std::byte, kFrameHeaderBytes> header{};
    std::vector<std::byte> payload;

    while (!stop_flag.load()) {
      const Outcome<void> got_header = connection.receive_exact(header, stop_flag, kPollMs);
      if (!got_header.has_value()) {
        return; // The peer closed, or the server is stopping.
      }

      MessageType type = MessageType::Hello;
      std::uint64_t request_id = 0;
      std::uint32_t payload_bytes = 0;
      const Outcome<void> decoded =
          decode_frame_header(header, limits.max_frame_payload_bytes, type, request_id, payload_bytes);
      if (!decoded.has_value()) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          ++stats.frames_rejected;
          ++stats.protocol_failures;
        }
        Encoder encoder;
        encode_failure(encoder, decoded.error(), limits);
        send_frame(connection, MessageType::Failure, request_id, encoder.view(),
                   limits.max_frame_payload_bytes, stop_flag, kPollMs);
        return;
      }

      payload.assign(payload_bytes, std::byte{0});
      if (payload_bytes != 0) {
        const Outcome<void> got_payload = connection.receive_exact(payload, stop_flag, kPollMs);
        if (!got_payload.has_value()) {
          return;
        }
      }

      Frame request;
      request.type = type;
      request.request_id = request_id;
      request.payload = payload;

      std::vector<std::byte> response;
      const MessageType response_type = dispatch(request, response);
      {
        std::lock_guard<std::mutex> lock(mutex);
        ++stats.requests_served;
      }
      send_frame(connection, response_type, request_id, response, limits.max_frame_payload_bytes, stop_flag,
                 kPollMs);

      if (response_type == MessageType::ShutdownAck) {
        {
          std::lock_guard<std::mutex> lock(mutex);
          shutdown_requested = true;
        }
        shutdown_requested_cv.notify_all();
        return;
      }
    }
  }

  void connection_thread(const std::shared_ptr<detail::Socket>& connection) {
    serve_connection(*connection);
    connection->close();
    {
      std::lock_guard<std::mutex> lock(mutex);
      active.erase(connection.get());
      --live_connections;
    }
    live_cv.notify_all();
  }

  void accept_loop() {
    while (true) {
      const Outcome<int> ready = detail::Socket::wait_readable(listener, wakeup_reader);
      if (!ready.has_value()) {
        return;
      }
      if (ready.value() == 1) {
        return; // The wakeup socket fired: the server is stopping.
      }
      Outcome<detail::Socket> accepted = listener.accept();
      if (!accepted.has_value()) {
        return;
      }
      auto connection = std::make_shared<detail::Socket>(std::move(accepted).value());
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || live_connections >= options.max_connections) {
          ++stats.connections_rejected;
          connection->shutdown();
          connection->close();
          continue;
        }
        ++stats.connections_accepted;
        ++live_connections;
        active.insert(connection.get());
      }
      try {
        std::thread([this, connection] { connection_thread(connection); }).detach();
      } catch (const std::system_error&) {
        std::lock_guard<std::mutex> lock(mutex);
        active.erase(connection.get());
        --live_connections;
        connection->close();
      }
    }
  }
};

Server::Server() : impl_(std::make_unique<Impl>()) {}

Server::~Server() {
  Stop();
}

std::uint16_t Server::port() const noexcept {
  return impl_->bound_port;
}

std::string Server::endpoint() const {
  return impl_->options.bind_address + ":" + std::to_string(impl_->bound_port);
}

ServerStats Server::stats() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->stats;
}

bool Server::wait_for_shutdown_request() {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  impl_->shutdown_requested_cv.wait(lock, [this] { return impl_->shutdown_requested || impl_->stopping; });
  return impl_->shutdown_requested;
}

Outcome<std::unique_ptr<Server>> Server::Start(Registry& registry, const ServerOptions& options) {
  Outcome<void> limits_valid = validate_limits(options.limits);
  if (!limits_valid.has_value()) {
    return make_error<std::unique_ptr<Server>>(limits_valid.error());
  }
  if (options.max_connections == 0 || options.listen_backlog == 0) {
    return make_error<std::unique_ptr<Server>>(
        ErrorCode::InvalidArgument, "max_connections and listen_backlog must both be greater than zero");
  }

  Outcome<void> sockets = detail::initialise_sockets();
  if (!sockets.has_value()) {
    return make_error<std::unique_ptr<Server>>(sockets.error());
  }

  std::unique_ptr<Server> server(new Server());
  server->impl_->registry = &registry;
  server->impl_->options = options;

  std::uint16_t bound_port = 0;
  Outcome<detail::Socket> listener =
      detail::Socket::listen_on(options.bind_address, options.port, options.listen_backlog, bound_port);
  if (!listener.has_value()) {
    return make_error<std::unique_ptr<Server>>(listener.error());
  }
  server->impl_->listener = std::move(listener).value();
  server->impl_->bound_port = bound_port;

  Outcome<std::pair<detail::Socket, detail::Socket>> wakeup = detail::Socket::make_pair();
  if (!wakeup.has_value()) {
    server->impl_->listener.close();
    return make_error<std::unique_ptr<Server>>(wakeup.error());
  }
  server->impl_->wakeup_writer = std::move(wakeup.value().first);
  server->impl_->wakeup_reader = std::move(wakeup.value().second);

  Impl* raw = server->impl_.get();
  try {
    raw->acceptor = std::thread([raw] { raw->accept_loop(); });
  } catch (const std::system_error& failure) {
    raw->listener.close();
    raw->wakeup_reader.close();
    raw->wakeup_writer.close();
    return make_error<std::unique_ptr<Server>>(
        ErrorCode::Internal, std::string("could not start the acceptor thread: ") + failure.what());
  }
  return server;
}

void Server::Stop() {
  if (!impl_) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopping = true;
  }
  impl_->stop_flag.store(true);
  impl_->shutdown_requested_cv.notify_all();

  // Waking the acceptor through its own socket is the one cross-thread wakeup
  // that is reliable on every platform; connection threads observe the stop
  // flag between I/O slices on their own.
  const Outcome<void> woken = impl_->wakeup_writer.wake();
  (void)woken;

  if (impl_->acceptor.joinable()) {
    impl_->acceptor.join();
  }
  {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->live_cv.wait(lock, [this] { return impl_->live_connections == 0; });
    impl_->active.clear();
  }
  impl_->listener.close();
  impl_->wakeup_reader.close();
  impl_->wakeup_writer.close();
}

} // namespace cable_registry::ipc
