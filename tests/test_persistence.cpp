// Cable Attachment Registry — persistence and recovery tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "registry_fixture.hpp"
#include "test_harness.hpp"

using namespace cable_registry;
using namespace crtest;

namespace {

const ObjectId kCable = object_id(300);
const ObjectId kOther = object_id(301);
const EndpointId kSwitch = endpoint_id(300);

std::vector<std::byte> read_file(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  const std::vector<char> characters((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(characters.size());
  for (std::size_t index = 0; index < characters.size(); ++index) {
    bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(characters[index]));
  }
  return bytes;
}

void write_file(const std::string& path, const std::vector<std::byte>& data) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

/// Populates a store with a small but complete topology and returns the graph
/// digest it reached.
Digest256 populate(const std::string& path, bool fsync = true) {
  RegistryOptions options;
  options.store = StoreOptions{};
  options.store->path = path;
  options.store->fsync_on_commit = fsync;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
  if (!opened.has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "open failed: " + opened.error().describe());
  }
  Registry& registry = *opened.value();
  Stream source{source_id(300), Authority{AuthorityClass::Observed, 5}};
  if (!registry.OpenSession(source.descriptor("agent"), Incarnation{1}).has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "session failed");
  }
  const std::vector<Evidence> records = {
      source.make(RegisterObjectPayload{make_object(kCable, "C-300")}),
      source.make(RegisterObjectPayload{make_object(kOther, "C-301")}),
      source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-300")}),
      source.make(attach(kCable, 0, kSwitch, 0)),
      source.make(attach(kCable, 1, kSwitch, 1)),
      source.make(attach(kOther, 0, kSwitch, 2)),
      source.make(detach(kOther, 0, true, kSwitch, 2)),
  };
  for (const Evidence& evidence : records) {
    Outcome<IngestResult> result = registry.Ingest(evidence);
    if (!result.has_value() || !result.value().accepted()) {
      ::crtest::fail(__FILE__, __LINE__, "ingest refused inside the fixture");
    }
  }
  Outcome<Digest256> digest = registry.GraphDigest();
  if (!digest.has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "digest failed");
  }
  const Digest256 value = digest.value();
  registry.Close();
  return value;
}

struct Opened {
  std::unique_ptr<Registry> registry;
  RecoveryReport report;
};

Outcome<Opened> open_store(const std::string& path, RecoveryPolicy policy = RecoveryPolicy::Strict) {
  RegistryOptions options;
  options.store = StoreOptions{};
  options.store->path = path;
  options.store->recovery_policy = policy;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
  if (!opened.has_value()) {
    return make_error<Opened>(opened.error());
  }
  Opened result;
  result.report = opened.value()->recovery_report();
  result.registry = std::move(opened).value();
  return result;
}

} // namespace

CR_TEST_CASE(persistence, a_closed_store_reopens_with_the_same_graph) {
  TempDir directory("reopen");
  const std::string path = directory.file("registry.log");
  const Digest256 before = populate(path);
  CR_CHECK(!read_file(path).empty());

  Outcome<Opened> reopened = open_store(path);
  CR_CHECK(reopened.has_value());
  CR_CHECK(reopened.value().report.created == false);
  CR_CHECK(reopened.value().report.issue == RecoveryIssue::CleanOpen);
  CR_CHECK(reopened.value().report.chain_intact);
  // One bootstrap state image, one session record and seven evidence records.
  CR_CHECK_EQ(reopened.value().report.records_loaded, std::uint64_t{9});
  CR_CHECK(reopened.value().registry->GraphDigest().value() == before);
  reopened.value().registry->Close();
}

CR_TEST_CASE(persistence, an_empty_store_is_created_and_usable) {
  TempDir directory("create");
  const std::string path = directory.file("registry.log");
  Outcome<Opened> opened = open_store(path);
  CR_CHECK(opened.has_value());
  CR_CHECK(opened.value().report.created);
  CR_CHECK(opened.value().report.issue == RecoveryIssue::Created);
  CR_CHECK_EQ(opened.value().report.records_loaded, std::uint64_t{0});
  opened.value().registry->Close();
  CR_CHECK(!read_file(path).empty());
}

CR_TEST_CASE(persistence, a_session_record_is_made_durable_before_it_is_acknowledged) {
  TempDir directory("crash");
  const std::string path = directory.file("registry.log");
  const Digest256 before = populate(path);

  // Simulate a crash: the process ends without a clean Close(). Everything the
  // registry acknowledged was flushed, so the whole graph survives.
  Outcome<Opened> reopened = open_store(path);
  CR_CHECK(reopened.has_value());
  CR_CHECK(reopened.value().registry->GraphDigest().value() == before);
  const Outcome<PortView> port = reopened.value().registry->QueryPort(port_of(kSwitch, 0));
  CR_CHECK(port.has_value());
  CR_CHECK(port.value().state == PortAttachmentState::Attached);
  CR_CHECK(port.value().validation == ValidationState::Unvalidated);
  reopened.value().registry->Close();
}

CR_TEST_CASE(persistence, a_torn_tail_is_truncated_and_the_prefix_survives) {
  TempDir directory("torn");
  const std::string path = directory.file("registry.log");
  populate(path);
  const std::vector<std::byte> full = read_file(path);
  // Drop the last few bytes of the final record: a half written append.
  const std::vector<std::byte> torn(full.begin(), full.end() - 6);
  write_file(path, torn);

  Outcome<Opened> strict = open_store(path);
  CR_CHECK(strict.has_value());
  CR_CHECK(strict.value().report.salvaged);
  CR_CHECK(strict.value().report.issue == RecoveryIssue::TruncatedTail);
  // The whole incomplete record is discarded, not just the missing bytes.
  CR_CHECK(strict.value().report.bytes_discarded > 6);
  CR_CHECK_EQ(strict.value().report.records_loaded, std::uint64_t{8});
  CR_CHECK(!strict.value().report.chain_intact);
  // The surviving prefix still holds six of the seven evidence records, so
  // five of the six ports are unchanged and the last one never happened.
  const Outcome<PortView> first = strict.value().registry->QueryPort(port_of(kSwitch, 0));
  CR_CHECK(first.has_value());
  CR_CHECK(first.value().state == PortAttachmentState::Attached);
  strict.value().registry->Close();

  // The file is left repaired, so a second open is clean.
  Outcome<Opened> again = open_store(path);
  CR_CHECK(again.has_value());
  CR_CHECK(!again.value().report.salvaged);
  CR_CHECK(again.value().report.issue == RecoveryIssue::CleanOpen);
  again.value().registry->Close();
}

CR_TEST_CASE(persistence, a_damaged_record_in_the_middle_is_refused_under_strict) {
  TempDir directory("damaged");
  const std::string path = directory.file("registry.log");
  populate(path);
  std::vector<std::byte> damaged = read_file(path);
  // Flip a bit well inside the log, away from the header and the tail.
  const std::size_t target = damaged.size() / 2;
  damaged[target] = static_cast<std::byte>(static_cast<unsigned>(damaged[target]) ^ 0x40u);
  write_file(path, damaged);

  Outcome<Opened> strict = open_store(path);
  CR_CHECK(!strict.has_value());
  CR_CHECK(strict.error().code() == ErrorCode::CorruptStore);

  // Salvaging keeps the intact prefix and discards everything from the damage.
  Outcome<Opened> salvaged = open_store(path, RecoveryPolicy::SalvagePrefix);
  CR_CHECK(salvaged.has_value());
  CR_CHECK(salvaged.value().report.salvaged);
  CR_CHECK(salvaged.value().report.bytes_discarded > 0);
  CR_CHECK(salvaged.value().report.records_loaded < 8);
  salvaged.value().registry->Close();
}

CR_TEST_CASE(persistence, a_damaged_header_is_refused_and_never_half_read) {
  TempDir directory("header");
  const std::string path = directory.file("registry.log");
  populate(path);
  std::vector<std::byte> damaged = read_file(path);
  damaged[1] = static_cast<std::byte>('X');
  write_file(path, damaged);

  Outcome<Opened> refused = open_store(path);
  CR_CHECK(!refused.has_value());
  CR_CHECK(refused.error().code() == ErrorCode::CorruptStore);

  // Even the salvage policy refuses: a header that is not a store is not a
  // store with a damaged tail.
  Outcome<Opened> salvaged = open_store(path, RecoveryPolicy::SalvagePrefix);
  CR_CHECK(!salvaged.has_value());
}

CR_TEST_CASE(persistence, an_unsupported_format_version_is_refused) {
  TempDir directory("version");
  const std::string path = directory.file("registry.log");
  populate(path);
  std::vector<std::byte> edited = read_file(path);
  // Rewrite the format version and repair the header checksum, so the only
  // thing wrong is the version itself.
  edited[8] = std::byte{99};
  edited[9] = std::byte{0};
  edited[10] = std::byte{0};
  edited[11] = std::byte{0};
  const std::uint32_t checksum = crc32c(std::span<const std::byte>(edited.data(), 20));
  for (int index = 0; index < 4; ++index) {
    edited[20 + static_cast<std::size_t>(index)] =
        static_cast<std::byte>((checksum >> (index * 8)) & 0xFFu);
  }
  write_file(path, edited);

  Outcome<Opened> refused = open_store(path, RecoveryPolicy::SalvagePrefix);
  CR_CHECK(!refused.has_value());
  CR_CHECK(refused.error().code() == ErrorCode::UnsupportedVersion);
}

CR_TEST_CASE(persistence, a_file_that_is_not_a_store_is_refused) {
  TempDir directory("garbage");
  const std::string path = directory.file("registry.log");
  std::vector<std::byte> garbage(4096);
  Rng rng(0xDEADBEEFull);
  for (std::byte& item : garbage) {
    item = static_cast<std::byte>(rng.next_u32() & 0xFFu);
  }
  write_file(path, garbage);
  Outcome<Opened> refused = open_store(path, RecoveryPolicy::SalvagePrefix);
  CR_CHECK(!refused.has_value());
}

CR_TEST_CASE(persistence, a_leftover_rewrite_target_is_removed_on_open) {
  TempDir directory("leftover");
  const std::string path = directory.file("registry.log");
  const Digest256 before = populate(path);
  write_file(path + ".tmp", read_file(path));
  CR_CHECK(std::ifstream(path + ".tmp").good());

  Outcome<Opened> opened = open_store(path);
  CR_CHECK(opened.has_value());
  CR_CHECK(!opened.value().report.salvaged);
  CR_CHECK(opened.value().registry->GraphDigest().value() == before);
  std::ifstream leftover(path + ".tmp");
  CR_CHECK(!leftover.good());
  opened.value().registry->Close();
}

CR_TEST_CASE(persistence, compaction_rewrites_the_log_and_preserves_the_graph) {
  TempDir directory("compact");
  const std::string path = directory.file("registry.log");
  const Digest256 before = populate(path);
  CR_CHECK(!read_file(path).empty());

  Outcome<Opened> opened = open_store(path);
  CR_CHECK(opened.has_value());
  CR_CHECK_EQ(opened.value().registry->Stats().store_records, std::uint64_t{9});
  const Outcome<void> compacted = opened.value().registry->Compact();
  CR_CHECK_MSG(compacted.has_value(), compacted.error().describe());
  CR_CHECK_EQ(opened.value().registry->Stats().store_records, std::uint64_t{1});
  opened.value().registry->Close();

  const std::vector<std::byte> after = read_file(path);
  CR_CHECK(after.size() > 32);
  CR_CHECK_EQ(static_cast<std::uint32_t>(after[8]), 1u); // one format version
  std::ifstream leftover(path + ".tmp");
  CR_CHECK(!leftover.good());

  Outcome<Opened> reopened = open_store(path);
  CR_CHECK(reopened.has_value());
  CR_CHECK_EQ(reopened.value().report.records_loaded, std::uint64_t{1});
  CR_CHECK(reopened.value().registry->GraphDigest().value() == before);
  // A compacted image carries no liveness: it is not fresh evidence.
  const Outcome<PortView> port = reopened.value().registry->QueryPort(port_of(kSwitch, 0));
  CR_CHECK(port.has_value());
  CR_CHECK(port.value().state == PortAttachmentState::Attached);
  CR_CHECK(port.value().validation == ValidationState::Unvalidated);
  reopened.value().registry->Close();

  // Compaction is refused on an in-memory registry rather than silently doing
  // nothing.
  RegistryOptions memory;
  Outcome<std::unique_ptr<Registry>> in_memory = Registry::Open(memory);
  CR_CHECK(in_memory.has_value());
  CR_CHECK(!in_memory.value()->Compact().has_value());
}

CR_TEST_CASE(persistence, compaction_bounds_the_log_against_repeated_updates) {
  TempDir directory("bounded-growth");
  const std::string path = directory.file("registry.log");
  RegistryOptions options;
  options.store = StoreOptions{};
  options.store->path = path;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
  CR_CHECK(opened.has_value());
  Registry& registry = *opened.value();
  Stream source{source_id(302), Authority{AuthorityClass::Observed, 5}};
  CR_CHECK(registry.OpenSession(source.descriptor("agent"), Incarnation{1}).has_value());
  CR_CHECK(registry.Ingest(source.make(RegisterObjectPayload{make_object(kCable, "C-320")})).has_value());
  CR_CHECK(registry.Ingest(source.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-320")})).has_value());

  for (std::uint32_t index = 0; index < 400; ++index) {
    const Outcome<IngestResult> result = registry.Ingest(source.make(attach(kCable, index % 2, kSwitch, index % 6)));
    CR_CHECK(result.has_value());
  }
  const std::uint64_t grown_bytes = registry.Stats().store_bytes;
  const std::uint64_t grown_records = registry.Stats().store_records;
  CR_CHECK(grown_records > 400);
  const Outcome<Digest256> before_compaction = registry.GraphDigest();
  CR_CHECK(before_compaction.has_value());

  CR_CHECK(registry.Compact().has_value());
  const std::uint64_t compacted_bytes = registry.Stats().store_bytes;
  CR_CHECK_EQ(registry.Stats().store_records, std::uint64_t{1});
  CR_CHECK_MSG(compacted_bytes * 4 < grown_bytes, "compaction did not bound the log against history");
  CR_CHECK(registry.GraphDigest().value() == before_compaction.value());
  registry.Close();

  Outcome<Opened> reopened = open_store(path);
  CR_CHECK(reopened.has_value());
  CR_CHECK(reopened.value().registry->GraphDigest().value() == before_compaction.value());
  CR_CHECK_EQ(reopened.value().report.records_loaded, std::uint64_t{1});
  reopened.value().registry->Close();
}

CR_TEST_CASE(persistence, evidence_that_does_not_fit_a_record_is_refused_and_not_applied) {
  TempDir directory("bounded");
  const std::string path = directory.file("registry.log");
  RegistryOptions options;
  options.store = StoreOptions{};
  options.store->path = path;
  options.store->max_record_bytes = 96;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
  CR_CHECK(opened.has_value());
  Registry& registry = *opened.value();

  Stream source{source_id(301), Authority{AuthorityClass::Observed, 5}};
  CR_CHECK(registry.OpenSession(source.descriptor("agent"), Incarnation{1}).has_value());
  ObjectDescriptor descriptor = make_object(kCable, "C-310");
  descriptor.physical_label.assign(200, 'x');
  const Outcome<IngestResult> refused = registry.Ingest(source.make(RegisterObjectPayload{descriptor}));
  CR_CHECK(refused.has_value());
  CR_CHECK(refused.value().disposition == IngestDisposition::RefusedPersistence);
  // Nothing was applied in memory either.
  CR_CHECK(!registry.QueryObject(kCable).has_value());
  registry.Close();
}

CR_TEST_CASE(persistence, the_registry_identity_is_stable_and_a_different_one_is_refused) {
  TempDir directory("identity");
  const std::string path = directory.file("registry.log");
  populate(path);

  RegistryId first{};
  {
    Outcome<Opened> opened = open_store(path);
    CR_CHECK(opened.has_value());
    first = opened.value().registry->id();
    CR_CHECK(!first.is_nil());
    opened.value().registry->Close();
  }
  {
    Outcome<Opened> opened = open_store(path);
    CR_CHECK(opened.has_value());
    CR_CHECK(opened.value().registry->id() == first);
    opened.value().registry->Close();
  }
  {
    RegistryOptions options;
    options.store = StoreOptions{};
    options.store->path = path;
    options.id = RegistryId::from_parts(0x1234, 0x5678);
    Outcome<std::unique_ptr<Registry>> conflicting = Registry::Open(options);
    CR_CHECK(!conflicting.has_value());
    CR_CHECK(conflicting.error().code() == ErrorCode::Conflict);
  }
}

CR_TEST_CASE(persistence, fsync_can_be_disabled_without_changing_the_recovered_graph) {
  TempDir directory("nofsync");
  const std::string path = directory.file("registry.log");
  const Digest256 before = populate(path, false);
  Outcome<Opened> reopened = open_store(path);
  CR_CHECK(reopened.has_value());
  CR_CHECK(reopened.value().registry->GraphDigest().value() == before);
  reopened.value().registry->Close();
}
