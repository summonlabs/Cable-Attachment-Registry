// Cable Attachment Registry — transport tests over real loopback sockets.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "registry_fixture.hpp"
#include "test_harness.hpp"
#include "test_process.hpp"

using namespace cable_registry;
using namespace crtest;

namespace {

const ObjectId kCable = object_id(900);
const EndpointId kSwitch = endpoint_id(900);

struct Running {
  std::unique_ptr<Registry> registry;
  std::unique_ptr<ipc::Server> server;
};

Running start_server() {
  RegistryOptions registry_options;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(registry_options);
  if (!opened.has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "registry open failed");
  }
  Running running;
  running.registry = std::move(opened).value();
  ipc::ServerOptions server_options;
  server_options.port = 0;
  server_options.max_connections = 16;
  Outcome<std::unique_ptr<ipc::Server>> server = ipc::Server::Start(*running.registry, server_options);
  if (!server.has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "server start failed: " + server.error().describe());
  }
  running.server = std::move(server).value();
  return running;
}

std::unique_ptr<ipc::Client> connect(std::uint16_t port) {
  ipc::ClientOptions options;
  options.host = "127.0.0.1";
  options.port = port;
  Outcome<std::unique_ptr<ipc::Client>> client = ipc::Client::Connect(options);
  if (!client.has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "client connect failed: " + client.error().describe());
  }
  return std::move(client).value();
}

} // namespace

CR_TEST_CASE(ipc, frame_round_trip_and_tamper_detection) {
  const std::vector<std::byte> payload = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const std::vector<std::byte> frame = ipc::encode_frame(ipc::MessageType::Publish, 42, payload, 1024);
  CR_CHECK_EQ(frame.size(), ipc::kFrameHeaderBytes + payload.size());

  const Outcome<ipc::Frame> decoded =
      ipc::decode_frame(std::span<const std::byte>(frame.data(), frame.size()), 1024);
  CR_CHECK(decoded.has_value());
  CR_CHECK(decoded.value().type == ipc::MessageType::Publish);
  CR_CHECK_EQ(decoded.value().request_id, std::uint64_t{42});
  CR_CHECK_EQ(decoded.value().payload.size(), payload.size());

  // Every single byte flip in the header must be detected.
  for (std::size_t index = 0; index < ipc::kFrameHeaderBytes; ++index) {
    std::vector<std::byte> damaged = frame;
    damaged[index] = static_cast<std::byte>(static_cast<unsigned>(damaged[index]) ^ 0x01u);
    CR_CHECK_MSG(!ipc::decode_frame(std::span<const std::byte>(damaged.data(), damaged.size()), 1024).has_value(),
                 "a mutated header byte was accepted at offset " + std::to_string(index));
  }
  // And in the payload.
  for (std::size_t index = ipc::kFrameHeaderBytes; index < frame.size(); ++index) {
    std::vector<std::byte> damaged = frame;
    damaged[index] = static_cast<std::byte>(static_cast<unsigned>(damaged[index]) ^ 0x80u);
    CR_CHECK(!ipc::decode_frame(std::span<const std::byte>(damaged.data(), damaged.size()), 1024).has_value());
  }
  // A frame longer than the receiver's bound is refused before any allocation.
  const std::vector<std::byte> wide = ipc::encode_frame(ipc::MessageType::Publish, 1, payload, 1024);
  CR_CHECK(!ipc::decode_frame(std::span<const std::byte>(wide.data(), wide.size()), 2).has_value());
  // An oversized payload cannot even be framed.
  Limits limits;
  limits.max_frame_payload_bytes = 2;
  CR_CHECK(ipc::encode_frame(ipc::MessageType::Publish, 1, payload, limits.max_frame_payload_bytes).empty());
}

CR_TEST_CASE(ipc, publish_and_query_over_loopback) {
  Running running = start_server();
  std::unique_ptr<ipc::Client> client = connect(running.server->port());
  CR_CHECK_EQ(client->hello().protocol_version, kProtocolVersion);
  CR_CHECK(client->hello().registry == running.registry->id());

  Stream source{source_id(900), Authority{AuthorityClass::Observed, 4}};
  const Outcome<OpenSessionResult> session = client->OpenSession(source.descriptor("remote"), Incarnation{1});
  CR_CHECK(session.has_value());
  CR_CHECK(session.value().disposition == OpenSessionResult::Disposition::Opened);

  CR_CHECK(client->Publish(source.make(RegisterObjectPayload{make_object(kCable, "C-900")})).value().accepted());
  CR_CHECK(client->Publish(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-900")})).value().accepted());
  const Outcome<IngestResult> attached = client->Publish(source.make(attach(kCable, 0, kSwitch, 2)));
  CR_CHECK(attached.has_value());
  CR_CHECK(attached.value().accepted());

  const Outcome<PortView> port = client->QueryPort(port_of(kSwitch, 2));
  CR_CHECK(port.has_value());
  CR_CHECK(port.value().state == PortAttachmentState::Attached);
  CR_CHECK_EQ(port.value().edges.size(), std::size_t{1});
  CR_CHECK(port.value().edges[0].subject.object == kCable);
  CR_CHECK(port.value().validation == ValidationState::Validated);

  const Outcome<ObjectView> object = client->QueryObject(kCable);
  CR_CHECK(object.has_value());
  CR_CHECK_EQ(object.value().physical_label, std::string("C-900"));

  const Outcome<TopologySnapshot> snapshot = client->Snapshot();
  CR_CHECK(snapshot.has_value());
  CR_CHECK(snapshot.value().graph_digest() == running.registry->GraphDigest().value());

  const Outcome<Digest256> digest = client->GraphDigest();
  CR_CHECK(digest.has_value());
  CR_CHECK(digest.value() == snapshot.value().graph_digest());

  const Outcome<RegistryStats> stats = client->Stats();
  CR_CHECK(stats.has_value());
  CR_CHECK_EQ(stats.value().objects, std::uint64_t{1});

  const Outcome<ObjectHistory> history = client->QueryHistory(kCable);
  CR_CHECK(history.has_value());
  CR_CHECK(!history.value().events.empty());

  const Outcome<InspectionView> inspection = client->Inspect(32);
  CR_CHECK(inspection.has_value());

  client->Close();
  running.server->Stop();
  running.registry->Close();
}

CR_TEST_CASE(ipc, a_failure_is_reported_as_a_typed_error_not_an_empty_success) {
  Running running = start_server();
  std::unique_ptr<ipc::Client> client = connect(running.server->port());

  const Outcome<PortView> missing = client->QueryPort(port_of(endpoint_id(1234), 0));
  CR_CHECK(!missing.has_value());
  CR_CHECK(missing.error().code() == ErrorCode::NotFound);

  const Outcome<ObjectView> unknown_object = client->QueryObject(object_id(4321));
  CR_CHECK(!unknown_object.has_value());
  CR_CHECK(unknown_object.error().code() == ErrorCode::NotFound);

  // A nil identity is refused. In process that is a typed disposition; over
  // the wire the transport decoder rejects it first, which the client reports
  // as a typed error. Either way it is never a silent success.
  const Outcome<OpenSessionResult> bad_session = client->OpenSession(SourceDescriptor{}, Incarnation{1});
  if (bad_session.has_value()) {
    CR_CHECK(bad_session.value().disposition == OpenSessionResult::Disposition::RefusedInvalid);
  } else {
    CR_CHECK(bad_session.error().code() == ErrorCode::InvalidArgument ||
             bad_session.error().code() == ErrorCode::ProtocolViolation);
  }

  client->Close();
  running.server->Stop();
  running.registry->Close();
}

CR_TEST_CASE(ipc, a_refusal_is_carried_back_to_the_publisher_intact) {
  Running running = start_server();
  std::unique_ptr<ipc::Client> client = connect(running.server->port());
  Stream source{source_id(901), Authority{AuthorityClass::Observed, 4}};
  CR_CHECK(client->OpenSession(source.descriptor("remote"), Incarnation{1}).has_value());

  // No session for this incarnation: the registry refuses the record and the
  // client sees the refusal, not a silent success.
  Stream other{source_id(902), Authority{AuthorityClass::Observed, 4}};
  const Outcome<IngestResult> refused = client->Publish(other.make(RegisterObjectPayload{make_object(kCable, "C-901")}));
  CR_CHECK(refused.has_value());
  CR_CHECK(refused.value().disposition == IngestDisposition::RefusedUnknownSource);
  CR_CHECK(!refused.value().accepted());

  // A malformed record is refused by the server with a typed error.
  Evidence malformed = source.make(RegisterObjectPayload{make_object(kCable, "C-902")});
  malformed.header.source = SourceId{};
  const Outcome<IngestResult> bad = client->Publish(malformed);
  if (bad.has_value()) {
    CR_CHECK(bad.value().disposition == IngestDisposition::RefusedInvalid);
    CR_CHECK(!bad.value().accepted());
  } else {
    CR_CHECK(bad.error().code() == ErrorCode::InvalidArgument ||
             bad.error().code() == ErrorCode::ProtocolViolation);
  }

  client->Close();
  running.server->Stop();
  running.registry->Close();
}

CR_TEST_CASE(ipc, concurrent_clients_are_serialised_by_the_registry) {
  Running running = start_server();
  constexpr std::uint32_t kClients = 4;
  std::vector<std::unique_ptr<ipc::Client>> clients;
  std::vector<Stream> sources;
  for (std::uint32_t index = 0; index < kClients; ++index) {
    clients.push_back(connect(running.server->port()));
    sources.emplace_back(source_id(910 + index), Authority{AuthorityClass::Observed, index});
    CR_CHECK(clients.back()->OpenSession(sources.back().descriptor("client"), Incarnation{1}).has_value());
  }
  // One object per client and port, so no source ever moves its own object and
  // every claim stays live.
  for (std::uint32_t index = 0; index < kClients; ++index) {
    for (std::uint32_t port = 0; port < 4; ++port) {
      CR_CHECK(clients[index]
                   ->Publish(sources[index].make(RegisterObjectPayload{
                       make_object(object_id(9100 + (index * 4) + port), "M-" + std::to_string(index) + "-" +
                                                                             std::to_string(port))}))
                   .value()
                   .accepted());
    }
  }
  Stream registrar{source_id(999), Authority{AuthorityClass::Observed, 0}};
  CR_CHECK(clients[0]->OpenSession(registrar.descriptor("registrar"), Incarnation{1}).has_value());
  CR_CHECK(clients[0]
               ->Publish(registrar.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-901", 16)}))
               .value()
               .accepted());

  std::vector<std::thread> threads;
  for (std::uint32_t index = 0; index < kClients; ++index) {
    threads.emplace_back([&clients, &sources, index] {
      for (std::uint32_t port = 0; port < 4; ++port) {
        const Outcome<IngestResult> result = clients[index]->Publish(
            sources[index].make(attach(object_id(9100 + (index * 4) + port), 0, kSwitch, (index * 4) + port)));
        if (!result.has_value() || !result.value().accepted()) {
          ::crtest::fail(__FILE__, __LINE__, "a concurrent publish was refused");
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  const Outcome<RegistryStats> stats = clients[0]->Stats();
  CR_CHECK(stats.has_value());
  CR_CHECK_EQ(stats.value().objects, std::uint64_t{kClients * 4});
  for (std::uint32_t index = 0; index < kClients; ++index) {
    const Outcome<PortView> port = clients[0]->QueryPort(port_of(kSwitch, index * 4));
    CR_CHECK(port.has_value());
    CR_CHECK(port.value().state == PortAttachmentState::Attached);
  }
  for (std::unique_ptr<ipc::Client>& client : clients) {
    client->Close();
  }
  running.server->Stop();
  running.registry->Close();
}

CR_TEST_CASE(ipc, stop_wakes_blocked_workers_and_joins_cleanly) {
  Running running = start_server();
  std::unique_ptr<ipc::Client> client = connect(running.server->port());
  Stream source{source_id(920), Authority{AuthorityClass::Observed, 1}};
  CR_CHECK(client->OpenSession(source.descriptor("idle"), Incarnation{1}).has_value());

  // The client is connected and idle. Stop must shut the listener and every
  // live connection down, join the workers, and return.
  std::thread stopper([&running] { running.server->Stop(); });
  stopper.join();
  CR_CHECK(running.server->stats().connections_accepted >= 1);

  // The client observes the closure as a failed call rather than a hang.
  const Outcome<RegistryStats> after = client->Stats();
  CR_CHECK(!after.has_value());
  client->Close();
  running.registry->Close();
}

CR_TEST_CASE(ipc, a_peer_that_speaks_nonsense_is_rejected_and_the_server_keeps_serving) {
  Running running = start_server();
  const std::uint16_t port = running.server->port();

  // Twenty-eight bytes of garbage: a header that cannot be trusted.
  std::vector<std::byte> garbage(ipc::kFrameHeaderBytes);
  Rng rng(0x1234ABCDull);
  for (std::byte& item : garbage) {
    item = static_cast<std::byte>(rng.next_u32() & 0xFFu);
  }
  std::vector<std::byte> reply;
  CR_CHECK(raw_exchange(port, garbage, reply));
  const Outcome<ipc::Frame> response =
      ipc::decode_frame(std::span<const std::byte>(reply.data(), reply.size()), 1u << 20);
  if (reply.empty()) {
    // Closing without a reply is also a correct refusal: the peer lost framing.
  } else {
    CR_CHECK(response.has_value());
    CR_CHECK(response.value().type == ipc::MessageType::Failure);
  }

  // A well formed client is still served afterwards.
  Stream source{source_id(930), Authority{AuthorityClass::Observed, 1}};
  std::unique_ptr<ipc::Client> valid = connect(port);
  CR_CHECK(valid->OpenSession(source.descriptor("valid"), Incarnation{1}).has_value());
  CR_CHECK(valid->Publish(source.make(RegisterObjectPayload{make_object(kCable, "C-930")})).value().accepted());
  const Outcome<RegistryStats> stats = valid->Stats();
  CR_CHECK(stats.has_value());
  const ipc::ServerStats server_stats = running.server->stats();
  CR_CHECK(server_stats.protocol_failures >= 1 || reply.empty());
  valid->Close();

  // A truncated frame is refused the same way.
  std::vector<std::byte> truncated = ipc::encode_frame(ipc::MessageType::Stats, 1, {}, 1u << 20);
  truncated.resize(ipc::kFrameHeaderBytes - 1);
  std::vector<std::byte> second_reply;
  CR_CHECK(raw_exchange(port, truncated, second_reply));

  running.server->Stop();
  running.registry->Close();
}
