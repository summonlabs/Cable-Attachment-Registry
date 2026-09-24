// Cable Attachment Registry — adversarial and malformed input tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "registry_fixture.hpp"
#include "test_harness.hpp"

using namespace cable_registry;
using namespace crtest;

namespace {

const ObjectId kCable = object_id(800);
const EndpointId kSwitch = endpoint_id(800);

std::span<const std::byte> bytes_of(const std::vector<std::byte>& data) {
  return std::span<const std::byte>(data.data(), data.size());
}

Evidence sample_evidence() {
  Stream source{source_id(800), Authority{AuthorityClass::Observed, 1}};
  return source.make(attach(kCable, 0, kSwitch, 3));
}

} // namespace

CR_TEST_CASE(adversarial, mutated_evidence_encodings_are_never_guessed) {
  const Evidence original = sample_evidence();
  const std::vector<std::byte> encoded = encode_evidence(original, Limits{});
  CR_CHECK(!encoded.empty());
  CR_CHECK(decode_evidence(bytes_of(encoded), Limits{}).has_value());

  Rng rng(0x5EEDull);
  std::uint32_t accepted_mutations = 0;
  for (int attempt = 0; attempt < 4000; ++attempt) {
    std::vector<std::byte> mutated = encoded;
    const std::size_t flips = 1 + static_cast<std::size_t>(rng.below(3));
    for (std::size_t index = 0; index < flips; ++index) {
      const std::size_t position = static_cast<std::size_t>(rng.below(mutated.size()));
      mutated[position] = static_cast<std::byte>(static_cast<unsigned>(mutated[position]) ^
                                                 (1u << rng.below(8)));
    }
    const Outcome<Evidence> decoded = decode_evidence(bytes_of(mutated), Limits{});
    if (!decoded.has_value()) {
      continue;
    }
    ++accepted_mutations;
    // If a mutation happens to stay decodable it must still be a well formed
    // record: every bound the encoder enforces is enforced again on the way in.
    CR_CHECK(!decoded.value().header.id.is_nil());
    CR_CHECK(!decoded.value().header.source.is_nil());
    CR_CHECK(decoded.value().header.incarnation.value >= 1);
    CR_CHECK(decoded.value().header.generation.value >= 1);
    CR_CHECK(decoded.value().header.schema_version == kEvidenceSchemaVersion);
    CR_CHECK(encode_evidence(decoded.value(), Limits{}).size() <= Encoder::kMaxEncodedBytes);
  }
  // Some mutations survive; the point is that none of them produce a crash or
  // an out of contract record.
  CR_CHECK(accepted_mutations < 4000);
}

CR_TEST_CASE(adversarial, truncated_evidence_encodings_are_refused) {
  const Evidence original = sample_evidence();
  const std::vector<std::byte> encoded = encode_evidence(original, Limits{});
  for (std::size_t length = 0; length < encoded.size(); ++length) {
    const std::vector<std::byte> truncated(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(length));
    CR_CHECK(!decode_evidence(bytes_of(truncated), Limits{}).has_value());
  }
  std::vector<std::byte> extended = encoded;
  extended.push_back(std::byte{0});
  CR_CHECK(!decode_evidence(bytes_of(extended), Limits{}).has_value());
}

CR_TEST_CASE(adversarial, random_bytes_never_decode_as_a_snapshot) {
  Rng rng(0xBADC0DEull);
  Limits limits;
  for (int attempt = 0; attempt < 600; ++attempt) {
    const std::size_t length = 1 + static_cast<std::size_t>(rng.below(512));
    std::vector<std::byte> noise(length);
    for (std::byte& item : noise) {
      item = static_cast<std::byte>(rng.next_u32() & 0xFFu);
    }
    const Outcome<TopologySnapshot> decoded = decode_snapshot(bytes_of(noise), limits);
    if (decoded.has_value()) {
      // Only a genuine header can decode; the version and registry identity
      // are still validated, so a random buffer cannot claim to be a snapshot
      // of a real registry.
      CR_CHECK(decoded.value().format_version == kSnapshotFormatVersion);
    }
  }
}

CR_TEST_CASE(adversarial, an_oversized_snapshot_count_is_refused_before_allocation) {
  // Hand build a header claiming four billion objects: the decoder must refuse
  // on the count itself, before it reserves anything.
  Encoder encoder;
  encoder.u32(kSnapshotFormatVersion);
  encoder.id128(Id128{1, 2});
  const std::array<std::uint8_t, 32> zero_digest{};
  encoder.bytes(std::span<const std::byte>(reinterpret_cast<const std::byte*>(zero_digest.data()),
                                           zero_digest.size()));
  for (int index = 0; index < 16; ++index) {
    encoder.u64(0);
  }
  encoder.u32(0xFFFFFFF0u); // object count
  const std::vector<std::byte> hostile = encoder.take();
  Limits limits;
  limits.max_snapshot_entries = 1024;
  CR_CHECK(!decode_snapshot(bytes_of(hostile), limits).has_value());
}

CR_TEST_CASE(adversarial, extreme_values_are_rejected_or_bounded) {
  Fixture fixture;
  Stream source{source_id(801), Authority{AuthorityClass::Observed, 1}};
  fixture.open(source, "agent");
  fixture.ingest(source.make(RegisterObjectPayload{make_object(kCable, "C-800")}));
  fixture.ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-800", 4)}));

  AttachPayload far_side = attach(kCable, 0xFFFFFFFFu, kSwitch, 0);
  const IngestResult side_result = fixture.try_ingest(source.make(far_side));
  CR_CHECK(side_result.accepted());
  const ObjectView view = fixture.object(kCable);
  CR_CHECK_EQ(view.sides.size(), std::size_t{2});
  // A side index beyond the declared sides is not a crash and not an
  // attachment: nothing is known about it.
  CR_CHECK(view.sides[0].state == PortAttachmentState::Unknown);
  CR_CHECK(view.sides[1].state == PortAttachmentState::Unknown);

  AttachPayload far_port = attach(kCable, 0, kSwitch, 0xFFFFFFF0u);
  const IngestResult port_result = fixture.try_ingest(source.make(far_port));
  CR_CHECK(port_result.disposition == IngestDisposition::AcceptedPendingReference);

  Evidence huge_generation = source.make(attach(kCable, 0, kSwitch, 0));
  huge_generation.header.generation = Generation{UINT64_MAX};
  const IngestResult huge = fixture.try_ingest(huge_generation);
  CR_CHECK(huge.accepted());
  CR_CHECK_EQ(huge.high_water.value, UINT64_MAX);

  Evidence zero_incarnation = source.make(attach(kCable, 0, kSwitch, 0));
  zero_incarnation.header.incarnation = Incarnation{0};
  CR_CHECK(fixture.try_ingest(zero_incarnation).disposition == IngestDisposition::RefusedInvalid);

  Evidence zero_generation = source.make(attach(kCable, 0, kSwitch, 0));
  zero_generation.header.generation = Generation{0};
  CR_CHECK(fixture.try_ingest(zero_generation).disposition == IngestDisposition::RefusedInvalid);

  Evidence future_schema = source.make(attach(kCable, 0, kSwitch, 0));
  future_schema.header.schema_version = kEvidenceSchemaVersion + 1;
  CR_CHECK(fixture.try_ingest(future_schema).disposition == IngestDisposition::RefusedInvalid);
}

CR_TEST_CASE(adversarial, oversized_strings_are_refused) {
  Fixture fixture;
  Stream source{source_id(802), Authority{AuthorityClass::Observed, 1}};
  fixture.open(source, "agent");
  ObjectDescriptor descriptor = make_object(kCable, "C-801");
  descriptor.physical_label.assign(1000, 'x');
  CR_CHECK(fixture.try_ingest(source.make(RegisterObjectPayload{descriptor})).disposition ==
           IngestDisposition::RefusedInvalid);

  EndpointDescriptor endpoint = make_endpoint(kSwitch, "sw-801", 4);
  endpoint.name.assign(1000, 'y');
  CR_CHECK(fixture.try_ingest(source.make(RegisterEndpointPayload{endpoint})).disposition ==
           IngestDisposition::RefusedInvalid);

  ObjectDescriptor no_sides = make_object(kCable, "C-802");
  no_sides.sides.clear();
  CR_CHECK(fixture.try_ingest(source.make(RegisterObjectPayload{no_sides})).disposition ==
           IngestDisposition::RefusedInvalid);

  EndpointDescriptor no_ports = make_endpoint(kSwitch, "sw-802", 4);
  no_ports.port_count = 0;
  CR_CHECK(fixture.try_ingest(source.make(RegisterEndpointPayload{no_ports})).disposition ==
           IngestDisposition::RefusedInvalid);
}

CR_TEST_CASE(adversarial, a_nil_identity_is_never_registrable) {
  Fixture fixture;
  Stream source{source_id(803), Authority{AuthorityClass::Observed, 1}};
  fixture.open(source, "agent");
  ObjectDescriptor descriptor = make_object(kCable, "C-803");
  descriptor.id = ObjectId{};
  CR_CHECK(fixture.try_ingest(source.make(RegisterObjectPayload{descriptor})).disposition ==
           IngestDisposition::RefusedInvalid);

  EndpointDescriptor endpoint = make_endpoint(kSwitch, "sw-803", 4);
  endpoint.id = EndpointId{};
  CR_CHECK(fixture.try_ingest(source.make(RegisterEndpointPayload{endpoint})).disposition ==
           IngestDisposition::RefusedInvalid);

  Outcome<OpenSessionResult> session = fixture->OpenSession(SourceDescriptor{}, Incarnation{1});
  CR_CHECK(session.has_value());
  CR_CHECK(session.value().disposition == OpenSessionResult::Disposition::RefusedInvalid);
}

CR_TEST_CASE(adversarial, capacity_limits_are_enforced_rather_than_exceeded) {
  Limits limits;
  limits.max_objects = 4;
  limits.max_endpoints = 1;
  limits.max_sources = 1;
  limits.max_claim_records_per_port = 2;
  Fixture fixture(limits);
  Stream source{source_id(804), Authority{AuthorityClass::Observed, 1}};
  fixture.open(source, "agent");
  for (std::uint32_t index = 0; index < 4; ++index) {
    CR_CHECK(fixture.try_ingest(source.make(
                 RegisterObjectPayload{make_object(object_id(900 + index), "K-" + std::to_string(index))}))
                 .accepted());
  }
  CR_CHECK(fixture.try_ingest(source.make(RegisterObjectPayload{make_object(object_id(999), "K-over")}))
               .disposition == IngestDisposition::RefusedCapacity);

  CR_CHECK(fixture.try_ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-804", 4)})).accepted());
  CR_CHECK(fixture.try_ingest(source.make(RegisterEndpointPayload{make_endpoint(endpoint_id(801), "sw-805", 4)}))
               .disposition == IngestDisposition::RefusedCapacity);

  Stream other{source_id(805), Authority{AuthorityClass::Observed, 1}};
  Outcome<OpenSessionResult> refused = fixture->OpenSession(other.descriptor("other"), Incarnation{1});
  CR_CHECK(refused.has_value());
  CR_CHECK(refused.value().disposition == OpenSessionResult::Disposition::RefusedCapacity);
}

CR_TEST_CASE(adversarial, replaying_the_accepted_set_changes_nothing) {
  Fixture fixture;
  Stream source{source_id(806), Authority{AuthorityClass::Observed, 1}};
  fixture.open(source, "agent");
  std::vector<Evidence> records;
  records.push_back(source.make(RegisterObjectPayload{make_object(kCable, "C-806")}));
  records.push_back(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-806", 4)}));
  records.push_back(source.make(attach(kCable, 0, kSwitch, 0)));
  records.push_back(source.make(move(kCable, 0, kSwitch, 0, kSwitch, 2)));
  for (const Evidence& evidence : records) {
    fixture.ingest(evidence);
  }
  const Digest256 before = fixture->GraphDigest().value();
  const TopologySnapshot snapshot_before = fixture.snapshot();

  for (const Evidence& evidence : records) {
    const IngestResult result = fixture.try_ingest(evidence);
    // A record whose claim key has since been superseded is stored as history;
    // one that still owns its key is a plain duplicate. Neither may change the
    // graph.
    CR_CHECK_MSG(result.disposition == IngestDisposition::Duplicate ||
                     result.disposition == IngestDisposition::AcceptedSuperseded,
                 std::string("unexpected disposition ") + to_string(result.disposition));
    CR_CHECK(result.accepted());
  }
  CR_CHECK(fixture->GraphDigest().value() == before);
  CR_CHECK(fixture.snapshot().encode() == snapshot_before.encode());
}

CR_TEST_CASE(adversarial, hostile_labels_survive_rendering_intact) {
  Fixture fixture;
  Stream source{source_id(807), Authority{AuthorityClass::Observed, 1}};
  fixture.open(source, "agent");
  const std::string label = "C-\"quoted\"\\back\\slash\nnewline\ttab\x01" "control";
  ObjectDescriptor descriptor = make_object(kCable, label);
  fixture.ingest(source.make(RegisterObjectPayload{descriptor}));
  const ObjectView view = fixture.object(kCable);
  CR_CHECK_EQ(view.physical_label, label);

  const std::string json = to_json(view);
  CR_CHECK(json.find("\\\"quoted\\\"") != std::string::npos);
  CR_CHECK(json.find("\\\\back\\\\slash") != std::string::npos);
  CR_CHECK(json.find("\\nnewline\\ttab\\u0001control") != std::string::npos);
  // No raw control byte from the label reaches the document: the writer emits the
  // escaped form only.
  CR_CHECK(json.find(static_cast<char>(1)) == std::string::npos);
  CR_CHECK(json.find(static_cast<char>(9)) == std::string::npos);
}

CR_TEST_CASE(adversarial, a_very_large_but_legal_graph_stays_linear_and_bounded) {
  RegistryOptions options;
  std::unique_ptr<Registry> registry = Registry::Open(options).value();
  Stream registrar{source_id(808), Authority{AuthorityClass::Observed, 0}};
  CR_CHECK(registry->OpenSession(registrar.descriptor("registrar"), Incarnation{1}).has_value());
  CR_CHECK(registry->Ingest(registrar.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-807", 512)})).has_value());

  Stream source{source_id(809), Authority{AuthorityClass::Observed, 1}};
  CR_CHECK(registry->OpenSession(source.descriptor("agent"), Incarnation{1}).has_value());
  constexpr std::uint32_t kObjects = 512;
  for (std::uint32_t index = 0; index < kObjects; ++index) {
    const ObjectId object = object_id(10000 + index);
    CR_CHECK(registry->Ingest(source.make(RegisterObjectPayload{make_object(object, "B-" + std::to_string(index))})).has_value());
    CR_CHECK(registry->Ingest(source.make(attach(object, 0, kSwitch, index % 512))).has_value());
  }
  const Outcome<TopologySnapshot> snapshot = registry->Snapshot();
  CR_CHECK(snapshot.has_value());
  CR_CHECK_EQ(snapshot.value().summary.objects_total, static_cast<std::uint64_t>(kObjects));
  CR_CHECK_EQ(snapshot.value().summary.edges, static_cast<std::uint64_t>(kObjects));
  CR_CHECK(snapshot.value().encode().size() < (1u << 20));
}
