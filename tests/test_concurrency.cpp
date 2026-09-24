// Cable Attachment Registry — concurrency tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Threads here are not a multiprocess proof; they are the proof that the
// runtime's own locking is sound. Real independent processes are exercised in
// test_multiprocess.cpp.

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "registry_fixture.hpp"
#include "test_harness.hpp"

using namespace cable_registry;
using namespace crtest;

namespace {

const EndpointId kSwitch = endpoint_id(600);

struct Prepared {
  std::vector<SourceDescriptor> descriptors;
  std::vector<std::vector<Evidence>> streams;
};

/// Builds @p sources independent streams of evidence that all fit the same
/// topology: each source owns its own objects and attaches them to distinct
/// ports, so the parallel run and the serial run must agree exactly.
Prepared prepare(std::uint32_t sources, std::uint32_t per_source) {
  Prepared prepared;
  for (std::uint32_t index = 0; index < sources; ++index) {
    Stream stream{source_id(600 + index), Authority{AuthorityClass::Observed, static_cast<std::uint32_t>(index)}};
    prepared.descriptors.push_back(stream.descriptor("source-" + std::to_string(index)));
    std::vector<Evidence> records;
    for (std::uint32_t item = 0; item < per_source; ++item) {
      const ObjectId object = object_id(6000 + (index * 100) + item);
      records.push_back(stream.make(
          RegisterObjectPayload{make_object(object, "L-" + std::to_string(index) + "-" + std::to_string(item))}));
      records.push_back(stream.make(attach(object, 0, kSwitch, index)));
    }
    prepared.streams.push_back(std::move(records));
  }
  return prepared;
}

/// A registry with the endpoint registered, ready for the sources above.
std::unique_ptr<Registry> make_registry() {
  RegistryOptions options;
  Outcome<std::unique_ptr<Registry>> opened = Registry::Open(options);
  if (!opened.has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "open failed");
  }
  std::unique_ptr<Registry> registry = std::move(opened).value();
  Stream registrar{source_id(699), Authority{AuthorityClass::Observed, 0}};
  if (!registry->OpenSession(registrar.descriptor("registrar"), Incarnation{1}).has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "session failed");
  }
  if (!registry->Ingest(registrar.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-600", 64)})).has_value()) {
    ::crtest::fail(__FILE__, __LINE__, "endpoint registration failed");
  }
  return registry;
}

} // namespace

CR_TEST_CASE(concurrency, parallel_publishers_reach_the_serial_graph) {
  const Prepared prepared = prepare(6, 12);

  std::unique_ptr<Registry> serial = make_registry();
  for (std::size_t index = 0; index < prepared.descriptors.size(); ++index) {
    CR_CHECK(serial->OpenSession(prepared.descriptors[index], Incarnation{1}).has_value());
    for (const Evidence& evidence : prepared.streams[index]) {
      CR_CHECK(serial->Ingest(evidence).value().accepted());
    }
  }
  const Digest256 expected = serial->GraphDigest().value();

  std::unique_ptr<Registry> parallel = make_registry();
  for (const SourceDescriptor& descriptor : prepared.descriptors) {
    CR_CHECK(parallel->OpenSession(descriptor, Incarnation{1}).has_value());
  }
  std::vector<std::thread> threads;
  std::atomic<int> refusals{0};
  threads.reserve(prepared.streams.size());
  for (const std::vector<Evidence>& stream : prepared.streams) {
    threads.emplace_back([&parallel, &stream, &refusals] {
      for (const Evidence& evidence : stream) {
        const Outcome<IngestResult> result = parallel->Ingest(evidence);
        if (!result.has_value() || !result.value().accepted()) {
          refusals.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  CR_CHECK_EQ(refusals.load(), 0);
  CR_CHECK(parallel->GraphDigest().value() == expected);
  CR_CHECK(parallel->Snapshot().value().summary == serial->Snapshot().value().summary);
}

CR_TEST_CASE(concurrency, readers_never_observe_a_torn_graph) {
  const Prepared prepared = prepare(4, 20);
  std::unique_ptr<Registry> registry = make_registry();
  for (const SourceDescriptor& descriptor : prepared.descriptors) {
    CR_CHECK(registry->OpenSession(descriptor, Incarnation{1}).has_value());
  }

  std::atomic<bool> stop{false};
  std::atomic<int> failures{0};
  std::atomic<std::uint64_t> observations{0};

  std::vector<std::thread> readers;
  for (int index = 0; index < 3; ++index) {
    readers.emplace_back([&registry, &stop, &failures, &observations] {
      while (!stop.load()) {
        const Outcome<TopologySnapshot> snapshot = registry->Snapshot();
        if (!snapshot.has_value()) {
          failures.fetch_add(1);
          continue;
        }
        for (const SnapshotPort& port : snapshot.value().ports) {
          if (port.state == PortAttachmentState::Attached || port.state == PortAttachmentState::Conflicting) {
            // An attached port always names at least one subject, and never
            // more than its capability admits.
            if (port.attachments.empty() ||
                port.attachments.size() > port.capability.max_simultaneous_attachments) {
              failures.fetch_add(1);
            }
          }
        }
        observations.fetch_add(1);
      }
    });
  }

  std::vector<std::thread> writers;
  for (const std::vector<Evidence>& stream : prepared.streams) {
    writers.emplace_back([&registry, &stream, &failures] {
      for (const Evidence& evidence : stream) {
        const Outcome<IngestResult> result = registry->Ingest(evidence);
        if (!result.has_value() || !result.value().accepted()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& writer : writers) {
    writer.join();
  }
  stop.store(true);
  for (std::thread& reader : readers) {
    reader.join();
  }
  CR_CHECK_EQ(failures.load(), 0);
  CR_CHECK(observations.load() > 0);
}

CR_TEST_CASE(concurrency, close_stops_new_work_without_deadlocking) {
  const Prepared prepared = prepare(4, 30);
  std::unique_ptr<Registry> registry = make_registry();
  for (const SourceDescriptor& descriptor : prepared.descriptors) {
    CR_CHECK(registry->OpenSession(descriptor, Incarnation{1}).has_value());
  }

  std::atomic<bool> closed_seen{false};
  std::atomic<int> published{0};
  std::vector<std::thread> threads;
  for (const std::vector<Evidence>& stream : prepared.streams) {
    threads.emplace_back([&registry, &stream, &closed_seen, &published] {
      for (const Evidence& evidence : stream) {
        const Outcome<IngestResult> result = registry->Ingest(evidence);
        if (!result.has_value()) {
          continue;
        }
        if (result.value().disposition == IngestDisposition::RefusedClosed) {
          closed_seen.store(true);
          return;
        }
        if (result.value().accepted()) {
          published.fetch_add(1);
        }
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  registry->Close();
  for (std::thread& thread : threads) {
    thread.join();
  }
  // Whatever the interleaving, the registry never accepts work after Close and
  // every later call is refused rather than silently dropped.
  const Outcome<IngestResult> after = registry->Ingest(prepared.streams.front().front());
  CR_CHECK(after.has_value());
  CR_CHECK(after.value().disposition == IngestDisposition::RefusedClosed);
  CR_CHECK(registry->closed());
  registry->Close(); // idempotent
  (void)closed_seen;
}

CR_TEST_CASE(concurrency, racing_publishers_never_produce_an_invalid_port_state) {
  RegistryOptions options;
  std::unique_ptr<Registry> registry = Registry::Open(options).value();
  Stream registrar{source_id(650), Authority{AuthorityClass::Observed, 0}};
  CR_CHECK(registry->OpenSession(registrar.descriptor("registrar"), Incarnation{1}).has_value());
  CR_CHECK(registry->Ingest(registrar.make(RegisterEndpointPayload{make_endpoint(kSwitch, "sw-601", 8)})).has_value());

  std::vector<Stream> sources;
  for (std::uint32_t index = 0; index < 8; ++index) {
    sources.emplace_back(source_id(700 + index), Authority{AuthorityClass::Observed, 1});
    CR_CHECK(registry->OpenSession(sources.back().descriptor("racer"), Incarnation{1}).has_value());
  }
  std::vector<ObjectId> objects;
  for (std::uint32_t index = 0; index < 8; ++index) {
    objects.push_back(object_id(7000 + index));
    CR_CHECK(registry->Ingest(sources[index].make(RegisterObjectPayload{make_object(objects.back(), "R-" + std::to_string(index))})).has_value());
  }

  std::vector<std::thread> threads;
  for (std::uint32_t index = 0; index < 8; ++index) {
    threads.emplace_back([&registry, &sources, &objects, index] {
      for (int round = 0; round < 40; ++round) {
        const std::uint32_t port = static_cast<std::uint32_t>((index + round) % 8);
        const Outcome<IngestResult> result =
            registry->Ingest(sources[index].make(attach(objects[index], 0, kSwitch, port)));
        if (!result.has_value()) {
          return;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  for (std::uint32_t port = 0; port < 8; ++port) {
    const PortView view = registry->QueryPort(port_of(kSwitch, port)).value();
    if (view.state == PortAttachmentState::Attached) {
      CR_CHECK_EQ(view.edges.size(), std::size_t{1});
      CR_CHECK(view.edges[0].claims.size() >= 1);
    }
    if (view.state == PortAttachmentState::Conflicting) {
      CR_CHECK(!view.conflicts.empty());
    }
  }
}
