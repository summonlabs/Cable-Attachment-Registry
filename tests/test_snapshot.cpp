// Cable Attachment Registry — snapshot determinism and arrival-order independence.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "registry_fixture.hpp"
#include "test_harness.hpp"

using namespace cable_registry;
using namespace crtest;

namespace {

const ObjectId kCableA = object_id(200);
const ObjectId kCableB = object_id(201);
const ObjectId kCableC = object_id(202);
const ObjectId kCableD = object_id(203);
const EndpointId kSwitchA = endpoint_id(200);
const EndpointId kSwitchB = endpoint_id(201);

/// A scenario is an ordered list of control-plane session opens plus a set of
/// evidence records that must produce the same graph in any order.
struct Scenario {
  std::vector<std::pair<SourceDescriptor, Incarnation>> sessions;
  std::vector<Evidence> records;
};

Scenario build_scenario() {
  Scenario scenario;
  Stream first{source_id(200), Authority{AuthorityClass::Observed, 10}};
  Stream second{source_id(201), Authority{AuthorityClass::Authoritative, 1}};
  scenario.sessions.emplace_back(first.descriptor("first"), first.incarnation());
  scenario.sessions.emplace_back(second.descriptor("second"), second.incarnation());

  scenario.records.push_back(first.make(RegisterObjectPayload{make_object(kCableA, "C-200")}));
  scenario.records.push_back(first.make(RegisterObjectPayload{make_object(kCableB, "C-201")}));
  scenario.records.push_back(first.make(RegisterObjectPayload{make_object(kCableC, "C-202")}));
  scenario.records.push_back(second.make(RegisterObjectPayload{make_object(kCableD, "C-203")}));
  scenario.records.push_back(first.make(RegisterEndpointPayload{make_endpoint(kSwitchA, "sw-200")}));
  scenario.records.push_back(second.make(RegisterEndpointPayload{make_endpoint(kSwitchB, "sw-201")}));

  scenario.records.push_back(first.make(attach(kCableA, 0, kSwitchA, 0)));
  scenario.records.push_back(first.make(attach(kCableA, 1, kSwitchA, 1)));
  scenario.records.push_back(first.make(attach(kCableB, 0, kSwitchB, 0)));
  scenario.records.push_back(second.make(attach(kCableD, 0, kSwitchB, 1)));
  scenario.records.push_back(first.make(detach(kCableB, 0, true, kSwitchB, 0)));
  scenario.records.push_back(first.make(move(kCableA, 0, kSwitchA, 0, kSwitchA, 2)));
  scenario.records.push_back(first.make(lifecycle(kCableC, LifecycleAction::Quarantined, "suspect")));

  ReincarnateObjectPayload reincarnate;
  reincarnate.object = kCableD;
  reincarnate.from_incarnation = ObjectIncarnation{1};
  reincarnate.new_serial_like = "SN-SWAP";
  scenario.records.push_back(second.make(reincarnate));

  PublishPortCapabilityPayload capability;
  capability.port = port_of(kSwitchA, 3);
  capability.capability.max_simultaneous_attachments = 2;
  scenario.records.push_back(first.make(capability));
  return scenario;
}

/// Feeds the scenario in the given order and returns the canonical graph
/// digest. Control-plane session opens always happen first: a record from a
/// source with no session is refused by design, so it is not part of the
/// "accepted evidence set" the property is about.
Digest256 run_order(const Scenario& scenario, const std::vector<std::size_t>& order, std::size_t* accepted) {
  RegistryOptions options;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
  if (!opened.has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "registry open failed");
  }
  Registry& registry = *opened.value();
  for (const auto& session : scenario.sessions) {
    Outcome<OpenSessionResult> result = registry.OpenSession(session.first, session.second);
    if (!result.has_value()) {
      ::crtest::fail(__FILE__, __LINE__, "session open failed");
    }
  }
  std::size_t stored = 0;
  for (const std::size_t index : order) {
    Outcome<IngestResult> result = registry.Ingest(scenario.records[index]);
    if (!result.has_value()) {
      ::crtest::fail(__FILE__, __LINE__, "ingest failed");
    }
    if (result.value().accepted()) {
      ++stored;
    }
  }
  if (accepted != nullptr) {
    *accepted = stored;
  }
  Outcome<Digest256> digest = registry.GraphDigest();
  if (!digest.has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "digest failed");
  }
  return digest.value();
}

} // namespace

CR_TEST_CASE(snapshot, graph_identity_is_independent_of_arrival_order) {
  const Scenario scenario = build_scenario();
  std::vector<std::size_t> order(scenario.records.size());
  std::iota(order.begin(), order.end(), 0);

  std::size_t baseline_accepted = 0;
  const Digest256 baseline = run_order(scenario, order, &baseline_accepted);
  CR_CHECK_EQ(baseline_accepted, scenario.records.size());

  std::vector<std::size_t> reversed(order.rbegin(), order.rend());
  std::size_t reversed_accepted = 0;
  CR_CHECK(run_order(scenario, reversed, &reversed_accepted) == baseline);
  CR_CHECK_EQ(reversed_accepted, scenario.records.size());

  Rng rng(0xC0FFEEull);
  for (int attempt = 0; attempt < 24; ++attempt) {
    std::vector<std::size_t> shuffled = order;
    for (std::size_t index = shuffled.size(); index > 1; --index) {
      const std::size_t swap = static_cast<std::size_t>(rng.below(index));
      std::swap(shuffled[index - 1], shuffled[swap]);
    }
    std::size_t accepted = 0;
    const Digest256 digest = run_order(scenario, shuffled, &accepted);
    CR_CHECK_MSG(digest == baseline, ("permutation " + std::to_string(attempt) + " produced a different graph"));
    CR_CHECK_MSG(accepted == baseline_accepted,
                 ("permutation " + std::to_string(attempt) + " stored a different number of records"));
  }
}

CR_TEST_CASE(snapshot, repeated_snapshots_of_one_registry_are_identical) {
  Fixture fixture;
  Stream source{source_id(202), Authority{AuthorityClass::Observed, 4}};
  fixture.open(source, "agent");
  fixture.ingest(source.make(RegisterObjectPayload{make_object(kCableA, "C-210")}));
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitchA, "sw-210")}));
  fixture.ingest(source.make(attach(kCableA, 0, kSwitchA, 0)));

  const TopologySnapshot first = fixture.snapshot();
  const TopologySnapshot second = fixture.snapshot();
  CR_CHECK(first.encode() == second.encode());
  CR_CHECK(first.digest() == second.digest());
  CR_CHECK(first.graph_digest() == second.graph_digest());
  CR_CHECK(!first.provenance_digest.is_zero());
}

CR_TEST_CASE(snapshot, encoding_round_trips) {
  Fixture fixture;
  Stream source{source_id(203), Authority{AuthorityClass::Observed, 4}};
  fixture.open(source, "agent");
  fixture.ingest(source.make(RegisterObjectPayload{make_object(kCableA, "C-211")}));
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitchA, "sw-211")}));
  fixture.ingest(source.make(attach(kCableA, 0, kSwitchA, 0)));
  fixture.ingest(source.make(attach(kCableA, 1, kSwitchA, 1)));

  const TopologySnapshot original = fixture.snapshot();
  const std::vector<std::byte> encoded = original.encode();
  const Outcome<TopologySnapshot> decoded = decode_snapshot(encoded, Limits{});
  CR_CHECK(decoded.has_value());
  CR_CHECK(decoded.value().encode() == encoded);
  CR_CHECK(decoded.value().digest() == original.digest());
  CR_CHECK(decoded.value().graph_digest() == original.graph_digest());
  CR_CHECK(decoded.value().summary == original.summary);

  // A truncated or edited image is refused rather than half-read.
  std::vector<std::byte> truncated(encoded.begin(), encoded.end() - 3);
  CR_CHECK(!decode_snapshot(truncated, Limits{}).has_value());
  std::vector<std::byte> extended = encoded;
  extended.push_back(std::byte{0});
  CR_CHECK(!decode_snapshot(extended, Limits{}).has_value());

  std::vector<std::byte> corrupted = encoded;
  corrupted[corrupted.size() / 2] = static_cast<std::byte>(static_cast<unsigned>(corrupted[corrupted.size() / 2]) ^ 0x5Au);
  const Outcome<TopologySnapshot> damaged = decode_snapshot(corrupted, Limits{});
  CR_CHECK(!damaged.has_value() || damaged.value().digest() != original.digest());
}

CR_TEST_CASE(snapshot, every_state_is_represented_without_collapsing) {
  Fixture fixture;
  Stream source{source_id(204), Authority{AuthorityClass::Observed, 4}};
  fixture.open(source, "agent");
  fixture.ingest(source.make(RegisterObjectPayload{make_object(kCableA, "C-212")}));
  fixture.ingest(source.make(RegisterObjectPayload{make_object(kCableB, "C-213")}));
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitchA, "sw-212")}));
  fixture.ingest(source.make(attach(kCableA, 0, kSwitchA, 0)));
  fixture.ingest(source.make(attach(kCableB, 0, kSwitchA, 1)));
  fixture.ingest(source.make(detach(kCableB, 0, true, kSwitchA, 1)));

  const TopologySnapshot snapshot = fixture.snapshot();
  const GraphSummary& summary = snapshot.summary;
  CR_CHECK_EQ(summary.ports_attached, std::uint64_t{1});
  CR_CHECK_EQ(summary.ports_empty, std::uint64_t{1});
  CR_CHECK_EQ(summary.ports_unknown, std::uint64_t{0});
  CR_CHECK_EQ(summary.objects_attached, std::uint64_t{1});
  CR_CHECK_EQ(summary.objects_unattached, std::uint64_t{1});
  CR_CHECK_EQ(summary.edges, std::uint64_t{1});

  // A port that was never observed does not appear at all, and a query for it
  // still answers UNKNOWN rather than inventing a state.
  for (const SnapshotPort& port : snapshot.ports) {
    CR_CHECK(!(port.port.index.value == 7));
  }
  const Outcome<PortView> never = fixture->QueryPort(port_of(kSwitchA, 7));
  CR_CHECK(never.has_value());
  CR_CHECK(never.value().state == PortAttachmentState::Unknown);
}

CR_TEST_CASE(snapshot, json_rendering_contains_the_digest_it_claims) {
  Fixture fixture;
  Stream source{source_id(205), Authority{AuthorityClass::Observed, 4}};
  fixture.open(source, "agent");
  fixture.ingest(source.make(RegisterObjectPayload{make_object(kCableA, "C-214")}));
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitchA, "sw-213")}));
  fixture.ingest(source.make(attach(kCableA, 0, kSwitchA, 0)));

  const TopologySnapshot snapshot = fixture.snapshot();
  const std::string json = to_json(snapshot);
  CR_CHECK(json.find(to_hex(snapshot.digest())) != std::string::npos);
  CR_CHECK(json.find(to_hex(snapshot.graph_digest())) != std::string::npos);
  CR_CHECK(json.find("C-214") != std::string::npos);
  CR_CHECK(json.size() > 64);
}
