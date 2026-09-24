// Cable Attachment Registry — publisher/query client.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// One request is in flight at a time. Every reply is matched against the
// request identifier that was sent, so a stale or reordered reply is refused
// instead of being handed back as the answer to a different question.

#include "cable_registry/ipc/client.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "cable_registry/codec.hpp"
#include "cable_registry/ipc/protocol.hpp"
#include "cable_registry/serialization.hpp"
#include "ipc/messages.hpp"
#include "ipc/socket.hpp"

namespace cable_registry::ipc {

struct Client::Impl {
  ClientOptions options;
  detail::Socket socket;
  Hello hello;
  std::uint64_t next_request_id = 1;
  bool closed = false;
};

Client::Client() : impl_(std::make_unique<Impl>()) {}

Client::~Client() {
  Close();
}

void Client::Close() {
  if (!impl_ || impl_->closed) {
    return;
  }
  impl_->closed = true;
  impl_->socket.shutdown();
  impl_->socket.close();
}

Outcome<std::unique_ptr<Client>> Client::Connect(const ClientOptions& options) {
  Outcome<void> limits_valid = validate_limits(options.limits);
  if (!limits_valid.has_value()) {
    return make_error<std::unique_ptr<Client>>(limits_valid.error());
  }
  if (options.port == 0) {
    return make_error<std::unique_ptr<Client>>(ErrorCode::InvalidArgument, "the client port must be set");
  }

  Outcome<void> sockets = detail::initialise_sockets();
  if (!sockets.has_value()) {
    return make_error<std::unique_ptr<Client>>(sockets.error());
  }
  Outcome<detail::Socket> socket = detail::Socket::connect(options.host, options.port);
  if (!socket.has_value()) {
    return make_error<std::unique_ptr<Client>>(socket.error());
  }

  std::unique_ptr<Client> client(new Client());
  client->impl_->options = options;
  client->impl_->socket = std::move(socket).value();

  // The greeting is exchanged immediately so a protocol mismatch is reported
  // at connect time rather than on the first real request.
  Encoder encoder;
  const std::vector<std::byte> greeting =
      encode_frame(MessageType::Hello, 0, encoder.view(), options.limits.max_frame_payload_bytes);
  Outcome<void> sent = client->impl_->socket.send_all(greeting);
  if (!sent.has_value()) {
    return make_error<std::unique_ptr<Client>>(sent.error());
  }

  std::array<std::byte, kFrameHeaderBytes> header{};
  Outcome<void> got_header = client->impl_->socket.receive_exact(header);
  if (!got_header.has_value()) {
    return make_error<std::unique_ptr<Client>>(got_header.error());
  }
  MessageType type = MessageType::Hello;
  std::uint64_t request_id = 0;
  std::uint32_t payload_bytes = 0;
  Outcome<void> decoded = decode_frame_header(header, options.limits.max_frame_payload_bytes, type,
                                              request_id, payload_bytes);
  if (!decoded.has_value()) {
    return make_error<std::unique_ptr<Client>>(decoded.error());
  }
  std::vector<std::byte> payload(payload_bytes, std::byte{0});
  if (payload_bytes != 0) {
    Outcome<void> got_payload = client->impl_->socket.receive_exact(payload);
    if (!got_payload.has_value()) {
      return make_error<std::unique_ptr<Client>>(got_payload.error());
    }
  }
  if (type == MessageType::Failure) {
    Decoder decoder(payload);
    Error error;
    if (decode_failure(decoder, options.limits, error)) {
      return make_error<std::unique_ptr<Client>>(error);
    }
    return make_error<std::unique_ptr<Client>>(ErrorCode::ProtocolViolation,
                                               "the registry refused the greeting");
  }
  if (type != MessageType::HelloAck || !decode_hello(payload, options.limits, client->impl_->hello)) {
    return make_error<std::unique_ptr<Client>>(ErrorCode::ProtocolViolation,
                                               "the registry sent an unexpected greeting");
  }
  if (client->impl_->hello.protocol_version != kProtocolVersion) {
    return make_error<std::unique_ptr<Client>>(
        ErrorCode::UnsupportedVersion,
        "the registry speaks protocol version " + std::to_string(client->impl_->hello.protocol_version) +
            " but this client speaks " + std::to_string(kProtocolVersion));
  }
  return client;
}

const Hello& Client::hello() const noexcept {
  return impl_->hello;
}

Outcome<Frame> Client::request(MessageType type, std::span<const std::byte> payload) {
  if (impl_->closed) {
    return make_error<Frame>(ErrorCode::Closed, "the client is closed");
  }
  const std::uint64_t request_id = impl_->next_request_id++;
  const std::vector<std::byte> request_frame =
      encode_frame(type, request_id, payload, impl_->options.limits.max_frame_payload_bytes);
  if (request_frame.empty()) {
    return make_error<Frame>(ErrorCode::CapacityExceeded, "the request does not fit in one frame");
  }
  Outcome<void> sent = impl_->socket.send_all(request_frame);
  if (!sent.has_value()) {
    return make_error<Frame>(sent.error());
  }

  std::array<std::byte, kFrameHeaderBytes> header{};
  Outcome<void> got_header = impl_->socket.receive_exact(header);
  if (!got_header.has_value()) {
    return make_error<Frame>(got_header.error());
  }
  MessageType response_type = MessageType::Failure;
  std::uint64_t response_id = 0;
  std::uint32_t payload_bytes = 0;
  Outcome<void> decoded = decode_frame_header(header, impl_->options.limits.max_frame_payload_bytes,
                                              response_type, response_id, payload_bytes);
  if (!decoded.has_value()) {
    return make_error<Frame>(decoded.error());
  }
  std::vector<std::byte> response_payload(payload_bytes, std::byte{0});
  if (payload_bytes != 0) {
    Outcome<void> got_payload = impl_->socket.receive_exact(response_payload);
    if (!got_payload.has_value()) {
      return make_error<Frame>(got_payload.error());
    }
  }
  if (response_id != request_id) {
    return make_error<Frame>(ErrorCode::ProtocolViolation,
                             "the reply identifier does not match the request that was sent");
  }
  if (response_type == MessageType::Failure) {
    Decoder decoder(response_payload);
    Error error;
    if (decode_failure(decoder, impl_->options.limits, error)) {
      return make_error<Frame>(error);
    }
    return make_error<Frame>(ErrorCode::ProtocolViolation, "the registry reported an unreadable failure");
  }
  Frame frame;
  frame.type = response_type;
  frame.request_id = response_id;
  frame.payload = std::move(response_payload);
  return frame;
}

Outcome<OpenSessionResult> Client::OpenSession(const SourceDescriptor& descriptor, Incarnation incarnation) {
  const std::vector<std::byte> payload =
      encode_open_session_request(descriptor, incarnation, impl_->options.limits);
  Outcome<Frame> frame = request(MessageType::OpenSession, payload);
  if (!frame.has_value()) {
    return make_error<OpenSessionResult>(frame.error());
  }
  if (frame.value().type != MessageType::OpenSessionAck) {
    return make_error<OpenSessionResult>(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  Decoder decoder(frame.value().payload);
  OpenSessionResult result;
  if (!decode_open_session_result(decoder, impl_->options.limits, result) || !decoder.done()) {
    return make_error<OpenSessionResult>(ErrorCode::ProtocolViolation, "the reply is malformed");
  }
  return result;
}

Outcome<void> Client::CloseSession(SourceIncarnationKey key) {
  Encoder encoder;
  encode_source_key(encoder, key);
  Outcome<Frame> frame = request(MessageType::CloseSession, encoder.view());
  if (!frame.has_value()) {
    return make_error(frame.error());
  }
  if (frame.value().type != MessageType::CloseSessionAck) {
    return make_error(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  return Outcome<void>();
}

Outcome<IngestResult> Client::Publish(const Evidence& evidence) {
  const std::vector<std::byte> payload = encode_publish_request(evidence, impl_->options.limits);
  Outcome<Frame> frame = request(MessageType::Publish, payload);
  if (!frame.has_value()) {
    return make_error<IngestResult>(frame.error());
  }
  if (frame.value().type != MessageType::PublishAck) {
    return make_error<IngestResult>(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  Decoder decoder(frame.value().payload);
  IngestResult result;
  if (!decode_ingest_result(decoder, impl_->options.limits, result) || !decoder.done()) {
    return make_error<IngestResult>(ErrorCode::ProtocolViolation, "the reply is malformed");
  }
  return result;
}

std::vector<IngestResult> Client::PublishBatch(std::span<const Evidence> evidence) {
  const Limits& limits = impl_->options.limits;
  const std::size_t take = std::min<std::size_t>(evidence.size(), limits.max_evidence_batch);
  std::vector<IngestResult> results;
  if (take == 0) {
    return results;
  }
  const std::vector<std::byte> payload = encode_publish_batch_request(evidence.first(take), limits);
  Outcome<Frame> frame = request(MessageType::PublishBatch, payload);
  const char* failure_detail = nullptr;
  if (!frame.has_value()) {
    failure_detail = "the publish-batch request failed";
  } else if (frame.value().type != MessageType::PublishBatchAck) {
    failure_detail = "unexpected reply type";
  }
  if (failure_detail != nullptr) {
    results.resize(take);
    for (IngestResult& result : results) {
      result.disposition = IngestDisposition::RefusedInvalid;
      result.detail = failure_detail;
    }
    return results;
  }
  Decoder decoder(frame.value().payload);
  std::vector<IngestResult> decoded;
  if (!decode_ingest_results(decoder, limits, decoded) || !decoder.done()) {
    results.resize(take);
    for (IngestResult& result : results) {
      result.disposition = IngestDisposition::RefusedInvalid;
      result.detail = "the reply is malformed";
    }
    return results;
  }
  return decoded;
}

Outcome<PortView> Client::QueryPort(PortRef port, ValidationPolicy policy) {
  const std::vector<std::byte> payload = encode_port_query(port, policy);
  Outcome<Frame> frame = request(MessageType::QueryPort, payload);
  if (!frame.has_value()) {
    return make_error<PortView>(frame.error());
  }
  if (frame.value().type != MessageType::QueryPortAck) {
    return make_error<PortView>(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  PortView view;
  if (!decode_port_response(frame.value().payload, impl_->options.limits, view)) {
    return make_error<PortView>(ErrorCode::ProtocolViolation, "the reply is malformed");
  }
  return view;
}

Outcome<EndpointView> Client::QueryEndpoint(EndpointId endpoint, bool include_ports, ValidationPolicy policy) {
  const std::vector<std::byte> payload = encode_endpoint_query(endpoint, include_ports, policy);
  Outcome<Frame> frame = request(MessageType::QueryEndpoint, payload);
  if (!frame.has_value()) {
    return make_error<EndpointView>(frame.error());
  }
  if (frame.value().type != MessageType::QueryEndpointAck) {
    return make_error<EndpointView>(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  EndpointView view;
  if (!decode_endpoint_response(frame.value().payload, impl_->options.limits, view)) {
    return make_error<EndpointView>(ErrorCode::ProtocolViolation, "the reply is malformed");
  }
  return view;
}

Outcome<ObjectView> Client::QueryObject(ObjectId object, const QueryOptions& options) {
  const std::vector<std::byte> payload = encode_object_query(object, options);
  Outcome<Frame> frame = request(MessageType::QueryObject, payload);
  if (!frame.has_value()) {
    return make_error<ObjectView>(frame.error());
  }
  if (frame.value().type != MessageType::QueryObjectAck) {
    return make_error<ObjectView>(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  ObjectView view;
  if (!decode_object_response(frame.value().payload, impl_->options.limits, view)) {
    return make_error<ObjectView>(ErrorCode::ProtocolViolation, "the reply is malformed");
  }
  return view;
}

Outcome<ObjectHistory> Client::QueryHistory(ObjectId object, std::uint32_t max_events) {
  const std::vector<std::byte> payload = encode_history_query(object, max_events);
  Outcome<Frame> frame = request(MessageType::QueryHistory, payload);
  if (!frame.has_value()) {
    return make_error<ObjectHistory>(frame.error());
  }
  if (frame.value().type != MessageType::QueryHistoryAck) {
    return make_error<ObjectHistory>(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  ObjectHistory history;
  if (!decode_history_response(frame.value().payload, impl_->options.limits, history)) {
    return make_error<ObjectHistory>(ErrorCode::ProtocolViolation, "the reply is malformed");
  }
  return history;
}

Outcome<InspectionView> Client::Inspect(std::uint32_t max_entries) {
  const std::vector<std::byte> payload = encode_inspect_query(max_entries);
  Outcome<Frame> frame = request(MessageType::Inspect, payload);
  if (!frame.has_value()) {
    return make_error<InspectionView>(frame.error());
  }
  if (frame.value().type != MessageType::InspectAck) {
    return make_error<InspectionView>(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  InspectionView view;
  if (!decode_inspect_response(frame.value().payload, impl_->options.limits, view)) {
    return make_error<InspectionView>(ErrorCode::ProtocolViolation, "the reply is malformed");
  }
  return view;
}

Outcome<TopologySnapshot> Client::Snapshot() {
  Outcome<Frame> frame = request(MessageType::Snapshot, {});
  if (!frame.has_value()) {
    return make_error<TopologySnapshot>(frame.error());
  }
  if (frame.value().type != MessageType::SnapshotAck) {
    return make_error<TopologySnapshot>(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  Decoder decoder(frame.value().payload);
  TopologySnapshot snapshot;
  if (!decode_topology_snapshot(decoder, impl_->options.limits, snapshot) || !decoder.done()) {
    return make_error<TopologySnapshot>(ErrorCode::ProtocolViolation, "the reply is malformed");
  }
  return snapshot;
}

Outcome<Digest256> Client::GraphDigest() {
  Outcome<Frame> frame = request(MessageType::Digest, {});
  if (!frame.has_value()) {
    return make_error<Digest256>(frame.error());
  }
  if (frame.value().type != MessageType::DigestAck) {
    return make_error<Digest256>(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  Digest256 digest;
  if (!decode_digest_response(frame.value().payload, digest)) {
    return make_error<Digest256>(ErrorCode::ProtocolViolation, "the reply is malformed");
  }
  return digest;
}

Outcome<RegistryStats> Client::Stats() {
  Outcome<Frame> frame = request(MessageType::Stats, {});
  if (!frame.has_value()) {
    return make_error<RegistryStats>(frame.error());
  }
  if (frame.value().type != MessageType::StatsAck) {
    return make_error<RegistryStats>(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  RegistryStats stats;
  if (!decode_stats_response(frame.value().payload, impl_->options.limits, stats)) {
    return make_error<RegistryStats>(ErrorCode::ProtocolViolation, "the reply is malformed");
  }
  return stats;
}

Outcome<void> Client::Shutdown() {
  Outcome<Frame> frame = request(MessageType::Shutdown, {});
  if (!frame.has_value()) {
    return make_error(frame.error());
  }
  if (frame.value().type != MessageType::ShutdownAck) {
    return make_error(ErrorCode::ProtocolViolation, "unexpected reply type");
  }
  return Outcome<void>();
}

} // namespace cable_registry::ipc
