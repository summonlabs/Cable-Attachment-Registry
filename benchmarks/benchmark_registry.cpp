// Cable Attachment Registry — benchmarks.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every number below measures completed useful work: an ingest figure counts
// records the registry accepted and made durable before returning, a query
// figure counts answers that were fully derived and returned, and the snapshot
// figure counts canonical images that were built and hashed. Nothing here
// measures submission latency or queue depth, and no figure says anything
// about hardware: the workload is synthetic and runs in one process.
//
// Run it with no arguments for the default sizes.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"

using namespace cable_registry;

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

struct Report {
  const char* name = "";
  double seconds = 0.0;
  std::uint64_t units = 0;
  const char* unit = "units";

  void print() const {
    const double rate = seconds > 0.0 ? static_cast<double>(units) / seconds : 0.0;
    std::printf("%-46s %10llu %-12s %9.3f s %14.0f /s\n", name, static_cast<unsigned long long>(units), unit,
                seconds, rate);
  }
};

std::string label_of(std::uint64_t index) {
  return "C-" + std::to_string(index);
}

ObjectId object_of(std::uint64_t index) {
  return ObjectId::from_parts(0x0B1EC70000000000ull, index + 1);
}

EndpointId endpoint_of(std::uint64_t index) {
  return EndpointId::from_parts(0x0E0D000000000000ull, index + 1);
}

struct Workload {
  std::uint64_t objects = 2000;
  std::uint64_t endpoints = 16;
  std::uint64_t ports_per_endpoint = 64;
};

/// Builds the evidence set once so both runs measure the same work.
std::vector<Evidence> build_records(const Workload& workload) {
  std::vector<Evidence> records;
  records.reserve(workload.objects + workload.endpoints + workload.objects);
  SourceId source = SourceId::from_parts(0x05000000000000AAull, 1);
  Generation generation{};
  auto make = [&](EvidencePayload payload) {
    generation.value += 1;
    Evidence evidence;
    evidence.header.id = EvidenceId::from_parts(0x00E71DE000000000ull, generation.value);
    evidence.header.source = source;
    evidence.header.incarnation = Incarnation{1};
    evidence.header.generation = generation;
    evidence.header.observed_at = Timestamp{static_cast<std::int64_t>(generation.value) * 1'000'000};
    evidence.header.provenance = ProvenanceClass::Synthetic;
    evidence.payload = std::move(payload);
    records.push_back(std::move(evidence));
  };

  for (std::uint64_t index = 0; index < workload.endpoints; ++index) {
    EndpointDescriptor endpoint;
    endpoint.id = endpoint_of(index);
    endpoint.kind = EndpointKind::Switch;
    endpoint.name = "sw-" + std::to_string(index);
    endpoint.port_count = static_cast<std::uint32_t>(workload.ports_per_endpoint);
    make(RegisterEndpointPayload{endpoint});
  }
  for (std::uint64_t index = 0; index < workload.objects; ++index) {
    ObjectDescriptor object;
    object.id = object_of(index);
    object.kind = ObjectKind::Cable;
    object.physical_label = label_of(index);
    object.sides.assign(2, ObjectSideDescriptor{ConnectorClass::Qsfp28, MediaClass::DirectAttachCopper,
                                                LaneCount{4}});
    make(RegisterObjectPayload{object});
  }
  for (std::uint64_t index = 0; index < workload.objects; ++index) {
    AttachPayload attachment;
    attachment.subject = ObjectSideRef{object_of(index), SideIndex{0}};
    attachment.port = PortRef{endpoint_of(index % workload.endpoints),
                              PortIndex{static_cast<std::uint32_t>(index % workload.ports_per_endpoint)}};
    attachment.object_incarnation = ObjectIncarnation{1};
    make(attachment);
  }
  return records;
}

} // namespace

int main(int argc, char** argv) {
  Workload workload;
  if (argc > 1) {
    workload.objects = static_cast<std::uint64_t>(std::strtoull(argv[1], nullptr, 10));
  }

  std::printf("Cable Attachment Registry %s — synthetic single-process workload\n", version_string().c_str());
  std::printf("objects=%llu endpoints=%llu ports-per-endpoint=%llu\n\n",
              static_cast<unsigned long long>(workload.objects),
              static_cast<unsigned long long>(workload.endpoints),
              static_cast<unsigned long long>(workload.ports_per_endpoint));

  const std::vector<Evidence> records = build_records(workload);
  const std::uint64_t registrations = workload.objects + workload.endpoints;
  const std::uint64_t attachments = workload.objects;

  std::printf("%-46s %10s %-12s %11s %16s\n", "measurement", "units", "unit", "elapsed", "throughput");

  // 1. Ingest with durability. Each accepted record is flushed before the call
  //    returns, so the measurement includes the cost of making it durable.
  {
    RegistryOptions options;
    Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
    if (!opened.has_value()) {
      return 1;
    }
    Registry& registry = *opened.value();
    SourceDescriptor descriptor;
    descriptor.id = SourceId::from_parts(0x05000000000000AAull, 1);
    descriptor.name = "benchmark";
    descriptor.authority.authority_class = AuthorityClass::Observed;
    if (!registry.OpenSession(descriptor, Incarnation{1}).has_value()) {
      return 1;
    }
    const auto start = Clock::now();
    std::uint64_t accepted = 0;
    for (const Evidence& evidence : records) {
      const Outcome<IngestResult> result = registry.Ingest(evidence);
      if (result.has_value() && result.value().accepted()) {
        ++accepted;
      }
    }
    Report report;
    report.name = "ingest accepted and applied (in memory)";
    report.seconds = seconds_since(start);
    report.units = accepted;
    report.unit = "records";
    report.print();

    Report query;
    query.name = "authoritative port queries answered";
    query.units = workload.objects;
    query.unit = "queries";
    const auto query_start = Clock::now();
    std::uint64_t answered = 0;
    for (std::uint64_t index = 0; index < workload.objects; ++index) {
      const Outcome<PortView> view = registry.QueryPort(
          PortRef{endpoint_of(index % workload.endpoints),
                  PortIndex{static_cast<std::uint32_t>(index % workload.ports_per_endpoint)}});
      if (view.has_value()) {
        ++answered;
      }
    }
    query.units = answered;
    query.seconds = seconds_since(query_start);
    query.print();

    Report snapshot;
    snapshot.name = "canonical snapshot built and hashed";
    snapshot.units = 20;
    snapshot.unit = "snapshots";
    const auto snapshot_start = Clock::now();
    Digest256 last{};
    for (int repeat = 0; repeat < 20; ++repeat) {
      const Outcome<TopologySnapshot> built = registry.Snapshot();
      if (!built.has_value()) {
        return 1;
      }
      last = built.value().graph_digest();
    }
    snapshot.seconds = seconds_since(snapshot_start);
    snapshot.print();
    std::printf("%-46s %s\n", "  final graph digest", to_hex(last).c_str());
    registry.Close();
  }

  std::printf("\n");
  const std::string store = "cable-registry-benchmark.log";
  std::remove(store.c_str());
  {
    RegistryOptions options;
    options.store = StoreOptions{};
    options.store->path = store;
    options.store->fsync_on_commit = true;
    Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
    if (!opened.has_value()) {
      return 1;
    }
    Registry& registry = *opened.value();
    SourceDescriptor descriptor;
    descriptor.id = SourceId::from_parts(0x05000000000000AAull, 1);
    descriptor.name = "benchmark";
    descriptor.authority.authority_class = AuthorityClass::Observed;
    if (!registry.OpenSession(descriptor, Incarnation{1}).has_value()) {
      return 1;
    }
    const auto start = Clock::now();
    std::uint64_t accepted = 0;
    for (const Evidence& evidence : records) {
      const Outcome<IngestResult> result = registry.Ingest(evidence);
      if (result.has_value() && result.value().accepted()) {
        ++accepted;
      }
    }
    Report report;
    report.name = "ingest accepted and flushed to stable storage";
    report.seconds = seconds_since(start);
    report.units = accepted;
    report.unit = "records";
    report.print();

    const auto compact_start = Clock::now();
    if (!registry.Compact().has_value()) {
      return 1;
    }
    Report compaction;
    compaction.name = "log compacted to one state image";
    compaction.seconds = seconds_since(compact_start);
    compaction.units = 1;
    compaction.unit = "rewrites";
    compaction.print();
    registry.Close();
  }
  std::remove(store.c_str());
  std::remove((store + ".tmp").c_str());

  std::printf("\nregistrations=%llu attachments=%llu\n",
              static_cast<unsigned long long>(registrations),
              static_cast<unsigned long long>(attachments));
  std::printf("All figures are synthetic in-process measurements of this build on this machine.\n");
  return 0;
}
