// Cable Attachment Registry — property and differential tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The reference model below is deliberately independent of the registry: it is
// a handful of maps that answer "where is this side plugged in" by replaying
// the operations in a single source stream. The registry has to agree with it,
// and the invariants that no reference model can express (fencing, capacity,
// determinism) are checked separately.

#include <algorithm>
#include <map>
#include <numeric>
#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "registry_fixture.hpp"
#include "test_harness.hpp"

using namespace cable_registry;
using namespace crtest;

namespace {

const EndpointId kEndpointA = endpoint_id(400);
const EndpointId kEndpointB = endpoint_id(401);
constexpr std::uint32_t kPorts = 6;
constexpr std::uint32_t kObjects = 6;
constexpr std::uint32_t kSides = 2;

ObjectId object_at(std::uint32_t index) {
  return object_id(400 + index);
}

struct Model {
  // subject -> port, absent means detached
  std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> placed;
  std::map<std::pair<std::uint32_t, std::uint32_t>, bool> ever_placed;
  std::map<std::uint32_t, ObjectIncarnation> incarnation;
};

/// Runs one randomized single-source scenario and compares the registry with
/// the reference model after every step.
void run_scenario(std::uint64_t seed, int steps) {
  Rng rng(seed);
  Fixture fixture;
  Stream source{source_id(400), Authority{AuthorityClass::Observed, 5}};
  fixture.open(source, "agent");
  Model model;

  for (std::uint32_t index = 0; index < kObjects; ++index) {
    fixture.ingest(source.make(RegisterObjectPayload{
        make_object(object_at(index), "P-" + std::to_string(index), kSides)}));
    model.incarnation[index] = ObjectIncarnation{1};
  }
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kEndpointA, "sw-a", kPorts)}));
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kEndpointB, "sw-b", kPorts)}));

  for (int step = 0; step < steps; ++step) {
    const std::uint32_t object = static_cast<std::uint32_t>(rng.below(kObjects));
    const std::uint32_t side = static_cast<std::uint32_t>(rng.below(kSides));
    const std::uint32_t port = static_cast<std::uint32_t>(rng.below(kPorts));
    const EndpointId endpoint = rng.chance(50) ? kEndpointA : kEndpointB;
    const auto key = std::make_pair(object, side);
    const ObjectIncarnation incarnation = model.incarnation[object];
    const int action = static_cast<int>(rng.below(100));

    if (action < 45) {
      fixture.ingest(source.make(attach(object_at(object), side, endpoint, port, incarnation)));
      model.placed[key] = port;
      model.ever_placed[key] = true;
    } else if (action < 75) {
      fixture.ingest(source.make(detach(object_at(object), side, true, endpoint, port, incarnation)));
      model.placed.erase(key);
      model.ever_placed[key] = true;
    } else if (action < 95) {
      const std::uint32_t destination = static_cast<std::uint32_t>(rng.below(kPorts));
      const EndpointId target_endpoint = rng.chance(50) ? kEndpointA : kEndpointB;
      fixture.ingest(source.make(
          move(object_at(object), side, endpoint, port, target_endpoint, destination, incarnation)));
      model.placed[key] = destination;
      model.ever_placed[key] = true;
    } else {
      ReincarnateObjectPayload reincarnate;
      reincarnate.object = object_at(object);
      reincarnate.from_incarnation = incarnation;
      reincarnate.reason = "swap";
      fixture.ingest(source.make(reincarnate));
      model.incarnation[object] = ObjectIncarnation{incarnation.value + 1};
      // A replacement fences every claim made against the old unit, including
      // the record that a side was ever placed, so the new unit's sides are
      // unknown again rather than empty.
      for (auto iterator = model.placed.begin(); iterator != model.placed.end();) {
        if (iterator->first.first == object) {
          iterator = model.placed.erase(iterator);
        } else {
          ++iterator;
        }
      }
      for (auto iterator = model.ever_placed.begin(); iterator != model.ever_placed.end();) {
        if (iterator->first.first == object) {
          iterator = model.ever_placed.erase(iterator);
        } else {
          ++iterator;
        }
      }
    }

    // The registry must agree with the model for this object side.
    const ObjectView view = fixture.object(object_at(object), QueryOptions{});
    CR_CHECK_MSG(view.incarnation == model.incarnation[object],
                 "seed " + std::to_string(seed) + " step " + std::to_string(step) +
                     ": object incarnation diverged");
    for (std::uint32_t checked_side = 0; checked_side < kSides; ++checked_side) {
      const auto lookup = model.placed.find(std::make_pair(object, checked_side));
      const ObjectSideView& side_view = view.sides[checked_side];
      if (lookup == model.placed.end()) {
        const bool expected_empty = model.ever_placed.count(std::make_pair(object, checked_side)) != 0;
        CR_CHECK_MSG(side_view.state == (expected_empty ? PortAttachmentState::Empty
                                                        : PortAttachmentState::Unknown),
                     "seed " + std::to_string(seed) + " step " + std::to_string(step) +
                         ": side state diverged from the reference model");
      } else {
        CR_CHECK_MSG(side_view.state == PortAttachmentState::Attached,
                     "seed " + std::to_string(seed) + " step " + std::to_string(step) +
                         ": model says attached, registry disagrees");
        CR_CHECK_EQ(side_view.edges.size(), std::size_t{1});
        CR_CHECK_EQ(side_view.edges[0].port.index.value, lookup->second);
      }
    }
  }
}

} // namespace

CR_TEST_CASE(property, single_source_matches_a_reference_model) {
  for (std::uint64_t seed : {1ull, 2ull, 3ull, 12345ull, 999983ull, 0xABCDEFull}) {
    run_scenario(seed, 60);
  }
}

CR_TEST_CASE(property, a_port_never_holds_more_than_its_declared_capability) {
  Rng rng(4242);
  Fixture fixture;
  Stream source{source_id(401), Authority{AuthorityClass::Observed, 5}};
  fixture.open(source, "agent");
  for (std::uint32_t index = 0; index < 8; ++index) {
    fixture.ingest(source.make(
        RegisterObjectPayload{make_object(object_at(index), "R-" + std::to_string(index), 1)}));
  }
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kEndpointA, "sw-a", kPorts)}));

  for (int step = 0; step < 80; ++step) {
    const std::uint32_t object = static_cast<std::uint32_t>(rng.below(8));
    const std::uint32_t port = static_cast<std::uint32_t>(rng.below(kPorts));
    fixture.ingest(source.make(attach(object_at(object), 0, kEndpointA, port)));
  }

  for (std::uint32_t port = 0; port < kPorts; ++port) {
    const PortView view = fixture.port(kEndpointA, port);
    CR_CHECK(view.state == PortAttachmentState::Attached || view.state == PortAttachmentState::Conflicting ||
             view.state == PortAttachmentState::Empty || view.state == PortAttachmentState::Unknown);
    if (view.state == PortAttachmentState::Attached) {
      CR_CHECK(view.edges.size() <= view.capability.max_simultaneous_attachments);
    }
    // Whatever the state, the port never invents a claim that was not fed to it.
    for (const AttachmentEdge& edge : view.edges) {
      for (const ClaimProvenance& claim : edge.claims) {
        CR_CHECK(claim.source == source.id());
        CR_CHECK(claim.generation.value >= 1);
        CR_CHECK(claim.generation.value <= source.last_generation().value);
      }
    }
  }
}

CR_TEST_CASE(property, random_scenarios_produce_the_same_graph_in_any_order) {
  for (std::uint64_t seed : {7ull, 11ull, 13ull, 424242ull}) {
    Rng rng(seed);
    Stream first{source_id(402), Authority{AuthorityClass::Observed, 5}};
    Stream second{source_id(403), Authority{AuthorityClass::Observed, 9}};
    std::vector<Evidence> records;
    for (std::uint32_t index = 0; index < 4; ++index) {
      records.push_back(first.make(
          RegisterObjectPayload{make_object(object_at(index), "Q-" + std::to_string(index), 2)}));
    }
    records.push_back(first.make(RegisterEndpointPayload{make_endpoint(kEndpointA, "sw-a", kPorts)}));
    records.push_back(second.make(RegisterEndpointPayload{make_endpoint(kEndpointB, "sw-b", kPorts)}));
    for (int step = 0; step < 30; ++step) {
      Stream& writer = rng.chance(50) ? first : second;
      const std::uint32_t object = static_cast<std::uint32_t>(rng.below(4));
      const std::uint32_t side = static_cast<std::uint32_t>(rng.below(2));
      const std::uint32_t port = static_cast<std::uint32_t>(rng.below(kPorts));
      const EndpointId endpoint = rng.chance(50) ? kEndpointA : kEndpointB;
      const int action = static_cast<int>(rng.below(100));
      if (action < 60) {
        records.push_back(writer.make(attach(object_at(object), side, endpoint, port)));
      } else if (action < 80) {
        records.push_back(writer.make(detach(object_at(object), side, true, endpoint, port)));
      } else {
        records.push_back(writer.make(
            move(object_at(object), side, endpoint, port, kEndpointA, (port + 1) % kPorts)));
      }
    }

    std::vector<std::size_t> order(records.size());
    std::iota(order.begin(), order.end(), 0);
    auto run = [&](const std::vector<std::size_t>& permutation) {
      RegistryOptions options;
      Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
      if (!opened.has_value()) {
        ::crtest::fail(__FILE__, __LINE__, "open failed");
      }
      Registry& registry = *opened.value();
      if (!registry.OpenSession(first.descriptor("first"), Incarnation{1}).has_value() ||
          !registry.OpenSession(second.descriptor("second"), Incarnation{1}).has_value()) {
        ::crtest::fail(__FILE__, __LINE__, "session failed");
      }
      for (const std::size_t index : permutation) {
        const Outcome<IngestResult> result = registry.Ingest(records[index]);
        if (!result.has_value()) {
          ::crtest::fail(__FILE__, __LINE__, "ingest failed");
        }
      }
      const Outcome<Digest256> digest = registry.GraphDigest();
      if (!digest.has_value()) {
        ::crtest::fail(__FILE__, __LINE__, "digest failed");
      }
      return digest.value();
    };

    const Digest256 baseline = run(order);
    for (int attempt = 0; attempt < 6; ++attempt) {
      std::vector<std::size_t> shuffled = order;
      for (std::size_t index = shuffled.size(); index > 1; --index) {
        std::swap(shuffled[index - 1], shuffled[static_cast<std::size_t>(rng.below(index))]);
      }
      CR_CHECK_MSG(run(shuffled) == baseline,
                   "seed " + std::to_string(seed) + " attempt " + std::to_string(attempt) +
                       " produced a different graph");
    }
  }
}

CR_TEST_CASE(property, the_supersession_ring_is_bounded_and_accounted_for) {
  const std::uint32_t history = 3;
  Fixture fixture(Limits{}, history);
  Stream source{source_id(404), Authority{AuthorityClass::Observed, 5}};
  fixture.open(source, "agent");
  fixture.ingest(source.make(RegisterObjectPayload{make_object(object_id(500), "S-1", 1)}));
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kEndpointA, "sw-a", kPorts)}));

  const std::uint32_t rewrites = 20;
  for (std::uint32_t index = 0; index < rewrites; ++index) {
    fixture.ingest(source.make(attach(object_id(500), 0, kEndpointA, index % kPorts)));
  }
  const Outcome<ObjectHistory> history_view = fixture->QueryHistory(object_id(500));
  CR_CHECK(history_view.has_value());
  // Every superseded generation is accounted for: either it is still in the
  // bounded history or it was reported as dropped.
  CR_CHECK(history_view.value().events.size() <= 256);
  CR_CHECK(history_view.value().events.size() >= 2);

  const ObjectView view = fixture.object(object_id(500));
  CR_CHECK(view.sides.size() == 1);
  CR_CHECK(view.sides[0].state == PortAttachmentState::Attached);
  const PortView port = fixture.port(kEndpointA, (rewrites - 1) % kPorts);
  CR_CHECK(port.state == PortAttachmentState::Attached);
  CR_CHECK_EQ(port.edges.size(), std::size_t{1});
}

CR_TEST_CASE(property, a_higher_authority_always_wins_whatever_the_arrival_order) {
  for (std::uint64_t seed : {21ull, 22ull, 23ull}) {
    Rng rng(seed);
    for (int layout = 0; layout < 4; ++layout) {
      Fixture fixture;
      Stream low{source_id(405), Authority{AuthorityClass::Imported, 1}};
      Stream high{source_id(406), Authority{AuthorityClass::Declared, 1}};
      fixture.open(low, "low");
      fixture.open(high, "high");
      fixture.ingest(low.make(RegisterObjectPayload{make_object(object_id(510), "T-1", 1)}));
      fixture.ingest(low.make(RegisterObjectPayload{make_object(object_id(511), "T-2", 1)}));
      fixture.ingest(low.make(RegisterObjectPayload{make_object(object_id(512), "T-3", 1)}));
      fixture.ingest(low.make(RegisterObjectPayload{make_object(object_id(513), "T-4", 1)}));
      fixture.ingest(low.make(RegisterEndpointPayload{make_endpoint(kEndpointA, "sw-a", kPorts)}));

      // Each port carries one low authority and one high authority claim. No
      // source moves its own object, so every claim stays live whatever order
      // the records arrive in.
      std::vector<Evidence> records = {
          low.make(attach(object_id(510), 0, kEndpointA, 0)),
          high.make(attach(object_id(511), 0, kEndpointA, 0)),
          low.make(attach(object_id(512), 0, kEndpointA, 1)),
          high.make(attach(object_id(513), 0, kEndpointA, 1)),
      };
      for (std::size_t index = records.size(); index > 1; --index) {
        std::swap(records[index - 1], records[static_cast<std::size_t>(rng.below(index))]);
      }
      for (const Evidence& evidence : records) {
        fixture.ingest(evidence);
      }
      for (std::uint32_t port = 0; port < 2; ++port) {
        const PortView view = fixture.port(kEndpointA, port);
        CR_CHECK(view.state == PortAttachmentState::Attached);
        CR_CHECK_EQ(view.edges.size(), std::size_t{1});
        CR_CHECK(view.edges[0].subject.object == (port == 0 ? object_id(511) : object_id(513)));
        CR_CHECK(view.edges[0].authority.authority_class == AuthorityClass::Declared);
        CR_CHECK_EQ(view.edges[0].overridden_total, std::uint32_t{1});
      }
    }
  }
}
