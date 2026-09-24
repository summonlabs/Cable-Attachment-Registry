# Cable Attachment Registry

A vendor-neutral runtime for physical attachment identity and topology
provenance across cables, patching, ports, endpoints and attachment lifecycle.

The registry answers one question, and answers it with evidence rather than
with a guess:

> What is physically attached to what, under which evidence and generation, and
> which source is authoritative enough to support that topology claim?

It is deliberately **not** a link-state system. It records what was observed or
declared about the physical plant, together with who said it, when they said
they saw it, which generation of their stream it belongs to, and which physical
incarnation of the object it was seen on. Everything else is somebody else's
runtime.

## What this runtime owns and what it does not

Implemented here:

- physical object registration (cables, patch cords, trunks, breakout harnesses,
  adapters, passive modules, loopbacks) with strongly typed identity, physical
  label, serial-like identity and administrative location;
- endpoint and port registration, with ports addressed only as (endpoint, index);
- attachment evidence ingestion: attach, detach, move, reincarnation, quarantine,
  release, removal, retirement, capability publication;
- an authority model that decides which source is authoritative enough to support
  a claim;
- an authoritative attachment query that reports Attached, Empty, Unknown or
  Conflicting per port and per object side, with every contributing claim cited;
- provenance and history queries;
- conflict, unknown and stale inspection;
- canonical, immutable topology snapshots with SHA-256 identity;
- versioned, integrity-checked persistence with conservative recovery;
- a framed loopback transport so publishers and consumers can be separate OS
  processes.

Not implemented, and not claimed:

- link state, link quality, reachability or routability;
- optical path activation, wavelength allocation or amplifier control;
- transceiver health, digital diagnostics or vendor telemetry;
- device programming, configuration push or firmware management;
- route computation, path selection or traffic engineering;
- any hardware, RDMA, InfiniBand, NVLink or switch/ASIC interaction whatsoever.

There is no device I/O in this repository. Every input is an evidence record
that some other system produced. The registry never claims to have measured a
cable.

## Concepts

**Physical object identity is not its label.** An object is identified by an
ObjectId, a 128 bit token chosen by the publisher. The label printed on the
cable (C-17), the serial-like string, and the administrative location are
attributes, not identity. Reusing a label of a live object is refused, which is
what stops evidence about a replaced unit from binding to its successor. The
label of a retired object may be reused.

**An attachment is a relation, not a property of an object.** Attach says "this
side of this object is plugged into this port". It binds the object side to a
port and, symmetrically, the port to the object side. Capability metadata (media
class, connector class, lane count, nominal rates) is separate, nominal, and
never promoted to operational state.

**Evidence is generation-bound.** Every record carries a source identity, the
incarnation of the publishing process, a monotonic generation inside that
incarnation, the observation time the source asserts, a provenance class, and a
unique evidence identifier. The registry stamps a receive time, but the receive
time never influences the graph.

**Ordering is by generation and authority, never by arrival.** Inside one source
incarnation the highest generation wins. Across sources, the highest authority
wins. If two sources at the same authority disagree, the registry reports
Conflicting and cites both. It never silently picks the record that arrived
last.

**Incarnations fence stale evidence.**

- A source that restarts presents a strictly higher incarnation. Everything the
  previous incarnation asserted about attachment state, lifecycle and
  capabilities is fenced and stops binding. Object, endpoint and capability
  registrations are catalogue facts about the plant and survive, which is why a
  port stays queryable after the agent that registered it restarted.
- A physical object that is replaced is reincarnated. Every claim recorded
  against the old incarnation is fenced and can never bind to the replacement;
  the new unit's sides are Unknown until evidence about them arrives.

**Recovered history stays historical.** A registry that restarted answers with
Unvalidated claims: they are reported, tagged, and excluded under the LiveOnly
policy until the owning source re-asserts them at a higher generation. Loading a
store never makes old evidence fresh.

**Lifecycle.** Registered, Unattached, Attached, Quarantined, Removed, Retired,
and Conflicting when two sources at equal authority assert incompatible states.
A move and a replacement are transitions recorded in the object history with
their own event kinds; the resting state after a move is Attached at the new
port. Retirement is sticky against another source of equal authority; a
reincarnation at the same authority restores the identity to service, and the
source that retired an object can correct itself inside its own monotonic
stream.

## Architecture

`cable_registry` is one dependency-free C++20 library. The headers under
`include/cable_registry/` are the public surface; everything under `src/` is
implementation.

| Area | Headers | Responsibility |
| --- | --- | --- |
| Identity | `ids.hpp`, `identity.hpp` | Strongly typed identities, ordinals, incarnations, object and endpoint descriptors |
| Vocabulary | `capability.hpp`, `authority.hpp` | Media, connector, object kinds, authority classes, provenance classes |
| Evidence | `evidence.hpp` | Record kinds, typed payloads, ingest outcomes |
| Runtime | `registry.hpp` | Registration, sessions, ingestion, queries, snapshots, compaction |
| Answers | `graph.hpp` | Port and object views, conflicts, validation state, inspection |
| Graph identity | `snapshot.hpp`, `digest.hpp` | Canonical encoding, SHA-256, CRC-32C |
| Durability | `persistence.hpp` | Versioned append-only log, recovery, atomic rewrite |
| Transport | `ipc/protocol.hpp`, `ipc/server.hpp`, `ipc/client.hpp` | Framed loopback protocol |
| Presentation | `render.hpp` | JSON rendering (a view, never an identity) |
| Encoding | `codec.hpp`, `serialization.hpp` | Bounded binary encoding of every domain value |

There are no third-party dependencies: C++20 and the standard library, plus
Winsock on Windows for the transport.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options (all default `ON` except the last two):

| Option | Meaning |
| --- | --- |
| `CABLE_ATTACHMENT_REGISTRY_BUILD_TESTS` | Build the test suites |
| `CABLE_ATTACHMENT_REGISTRY_BUILD_TOOLS` | Build `cable-registry-daemon` and `cable-registry-cli` |
| `CABLE_ATTACHMENT_REGISTRY_BUILD_EXAMPLES` | Build the example programs |
| `CABLE_ATTACHMENT_REGISTRY_BUILD_BENCHMARKS` | Build the benchmark program |
| `CABLE_ATTACHMENT_REGISTRY_WARNINGS_AS_ERRORS` | /WX on MSVC, -Werror elsewhere (default `ON`) |
| `CABLE_ATTACHMENT_REGISTRY_SANITIZERS` | AddressSanitizer (default `OFF`) |
| `CABLE_ATTACHMENT_REGISTRY_BUILD_PACKAGE_CONSUMER_TEST` | Add the install and find_package test (default `ON`) |

The library is built warning-clean with /W4 /WX on MSVC and
-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror
elsewhere. No CTest test sets a timeout; a hanging test is treated as a defect,
and the suite has none.

## Installing and consuming

```
cmake --install build --prefix /some/prefix
```

A downstream project then uses the exported package:

```cmake
find_package(CableAttachmentRegistry 1.0 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE SummonSoftwareLabs::CableAttachmentRegistry)
```

`tests/package_consumer` is exactly such a project. It is not part of the library
build: it is configured on its own against an installed prefix, and the CTest
case `cable-registry-test-package-consumer` runs that install, configure, build
and execution cycle against the freshly built tree.

## Embedding

```cpp
#include <cable_registry/cable_attachment_registry.hpp>
using namespace cable_registry;

RegistryOptions options;
options.store = StoreOptions{};      // omit for an in-memory registry
options.store->path = "registry.log";
auto registry = Registry::Open(options).value();

SourceDescriptor source;
source.id = SourceId(Id128{0x0500000000000001ull, 1});
source.name = "discovery-agent";
source.authority.authority_class = AuthorityClass::Observed;
source.authority.rank = 10;
registry->OpenSession(source, Incarnation{1});

Evidence evidence;
evidence.header.id = EvidenceId(Id128{0x00E71DE000000000ull, 1});
evidence.header.source = source.id;
evidence.header.incarnation = Incarnation{1};
evidence.header.generation = Generation{1};
evidence.header.observed_at = now_timestamp();
evidence.header.provenance = ProvenanceClass::DiscoveryAgent;
evidence.payload = AttachPayload{ObjectSideRef{cable, SideIndex{0}},
                                 PortRef{panel, PortIndex{3}},
                                 ObjectIncarnation{1}};
registry->Ingest(evidence);

PortView port = registry->QueryPort(PortRef{panel, PortIndex{3}}).value();
```

`examples/embed_registry.cpp`, `examples/publish_over_tcp.cpp` and
`examples/conflict_inspection.cpp` are complete programs.

## Command line tools

`cable-registry-daemon` hosts one registry over the framed loopback transport.
It prints `READY <port>` on standard output once the listener is bound and stops
when a client asks it to:

```
cable-registry-daemon --store registry.log --port 0
```

`cable-registry-cli` works against a local store or against a running daemon:

```
cable-registry-cli --store registry.log script evidence.script
cable-registry-cli --endpoint 127.0.0.1:5000 query-port <endpoint-hex> 3
cable-registry-cli --endpoint 127.0.0.1:5000 digest
cable-registry-cli --store registry.log snapshot --out graph.bin
```

`--time-base <unix-nanos>` derives observation times from a fixed base instead of
the wall clock, which makes a scripted publication reproducible and lets two
independent runs produce the same canonical digest. Without it the CLI stamps
the real clock. Run either tool with `--help` for the full command list.

## Transport

Frames are little endian: a 28 byte header followed by the payload.

```
magic u32 | protocol u16 | type u16 | payload bytes u32 | request id u64
| payload crc32c u32 | header crc32c u32
```

The header checksum is what lets a reader that has lost framing close the
connection instead of guessing at a length. Payloads are bounded by
Limits::max_frame_payload_bytes (1 MiB by default) and refused before any
allocation. A reply is matched against the request identifier that was sent, so
a stale or reordered reply is refused rather than handed back as the answer to a
different question. Failures travel as typed Failure frames and never as an empty
success.

Each connection is served by its own thread, bounded by
ServerOptions::max_connections; a connection beyond the bound is refused rather
than queued, because queueing a persistent publisher would block it
indefinitely. Shutdown sets a stop flag, wakes the acceptor through a dedicated
wakeup socket, and waits for the live connection count to reach zero. The
connection loops never block indefinitely inside the operating system: they wait
for readiness in 25 ms slices and check the stop flag between slices. Nothing is
cancelled from another thread, which Windows does not support for a blocked
receive, and no connection outlives the server.

## Persistence and recovery

The log is a header plus framed records, each with a CRC-32C, a sequence number,
and a running SHA-256 chain over the previous chain value and the record. The
record checksum catches a torn or bit-rotted record; the chain catches a removed,
reordered or spliced one. Neither is a signature: the store detects accidental
damage, it does not authenticate the writer.

Store::Open scans and validates the whole log before anything is served:

- a clean log opens with a CleanOpen report;
- an incomplete final record, the signature of a crash mid-append, is truncated
  away and reported as TruncatedTail, because refusing to open a store that a
  crash tore would turn an expected event into data loss;
- damage anywhere else is fatal under the default Strict policy. The
  SalvagePrefix policy keeps the intact prefix and reports exactly how many bytes
  were discarded;
- a bad magic, a bad header checksum, or an unsupported format version is always
  fatal;
- a leftover rewrite target is removed on open, because it was never committed.

Registry::Compact() rewrites the log as one state image through a temporary file
and an atomic rename. The handle on the log is released before the rename so no
platform has to replace a file this process still holds open. A crash between the
two leaves the original log intact.

Every acknowledged record is flushed to stable storage before the call returns
when StoreOptions::fsync_on_commit is set, which is the default. With it cleared,
a record survives a process crash but not a power loss; both are tested.

## Determinism and the canonical snapshot

TopologySnapshot is an immutable image of the authoritative graph. Its canonical
encoding contains nothing that varies with arrival order, wall clock or registry
lifetime:

- no receive timestamps and no validation state anywhere;
- no sequence numbers and no counters of the registry itself;
- collections sorted into canonical order at encode time, so a snapshot
  assembled by hand is canonicalised too.

Lifetime-dependent counters, such as the number of unvalidated edges, are
deliberately zero in TopologySnapshot::summary; SnapshotEnvelope carries them
instead, so that two registries holding the same evidence agree on the canonical
bytes.

graph_digest() is the SHA-256 of the encoding with the registry identity
excluded. Two independent registries that accepted the same evidence set produce
the same graph_digest() whatever order the records arrived in, and
provenance_digest is a digest over the accepted evidence itself. The arrival-order
property is tested in process over permutations and across real processes in
test_multiprocess.cpp.

## Bounded resources

Every externally derived size is validated before it is used. Limits bounds
objects, endpoints, ports per endpoint, sides per object, sources, attachments
per port, claim records per port, supersession history per claim key, string
lengths, evidence schemas, batch sizes, frame payloads, snapshot entries and the
refusal audit. A registry that would exceed a bound refuses the work with
RefusedCapacity or RefusedInvalid instead of growing without limit. The encoder
refuses to grow past 8 MiB and latches a failure rather than emitting a partially
written field.

## Tests

Eleven suites, all dependency-free and all timeout-free:

| Suite | What it proves |
| --- | --- |
| `test_foundation` | Identity round trips, SHA-256 and CRC-32C against known vectors, codec bounds, policy validation |
| `test_registry` | Registration, attach, detach, move, lifecycle, label reuse, reincarnation, source fencing, breakout ports |
| `test_authority` | Authority precedence, conflict preservation, LiveOnly, inspection, connector compatibility, pending references |
| `test_snapshot` | Arrival-order independence over permutations, snapshot round trip, digest stability, canonical counters |
| `test_persistence` | Close and reopen, torn tails, mid-file damage, bad headers, unsupported versions, stray temporaries, compaction and bounded growth |
| `test_property` | A randomised single-source scenario checked step by step against an independent reference model, plus invariants and permutation equivalence |
| `test_concurrency` | Parallel publishers reaching the serial graph, torn-read freedom, shutdown, racing publishers |
| `test_adversarial` | Mutated and truncated encodings, malformed snapshots, extreme values, capacity, replay, hostile label rendering, a large graph |
| `test_ipc` | Frame round trip and tamper detection on every byte, loopback publication and query, typed failures, concurrent clients, shutdown, protocol violations |
| `test_multiprocess` | Independent OS processes: concurrent publishers reaching the same graph, a hard kill followed by restart, a restarted publisher fencing its previous incarnation, a damaged store refused by a fresh process |
| `cable-registry-test-package-consumer` | Installing the package and building and running a standalone find_package consumer |

Threads are not used as a multiprocess proof anywhere. The process tests start
real executables, read their readiness line from a pipe, kill them outright at
materially distinct lifecycle boundaries, and restart them.

## Benchmarks

`cable-registry-benchmarks` measures completed useful work only: accepted records
the registry applied or made durable before returning, derived query answers, and
canonical snapshots that were actually built and hashed. Nothing measures
submission latency or queue depth, and no figure says anything about hardware.
The workload is synthetic and runs in one process.

Measured numbers for this build on this machine are recorded in `VALIDATION.md`
together with how they were obtained.

## Adjacent runtimes

This registry is a producer of provenance, not a consumer of it. Fabric topology,
optical path planning, link state, transceiver health and route computation all
live elsewhere; they read canonical snapshots and digests from here and remain
responsible for their own decisions. Nothing in this repository opens a device,
configures a port, or decides whether a link is up.

## Limitations

These are real and worth knowing before relying on the runtime.

- **A single writer per store.** One process owns a log file. There is no
  distributed consensus, no leader election and no multi-writer coordination.
  Two registries over one file is not supported and is not detected.
- **The transport is loopback and unauthenticated.** Any local process that can
  reach the port can publish. Bind it to a loopback address, as the daemon does
  by default, and put it behind whatever local access control the host has.
- **Integrity, not authenticity.** CRC-32C and SHA-256 detect accidental damage.
  They do not authenticate a publisher and do not make a store tamper-evident
  against a writer who can rewrite the whole chain.
- **Authority is declared, not derived.** The registry orders claims by the
  authority class and rank an operator assigned to a source. It has no opinion
  about whether that assignment is right.
- **Retirement has one deliberate escape hatch.** The source that retired an
  object can restore it by asserting it present at a higher generation of the
  same stream, and a reincarnation at equal authority restores it. Everything
  else at equal authority stays retired.
- **Snapshot export over the transport is frame-bounded.** A snapshot that does
  not fit one frame is refused rather than truncated; narrow the query or raise
  max_frame_payload_bytes.
- **Queries are linear in the state.** A port or object query touches only the
  records for that port or object, but a whole-topology query and a snapshot are
  O(state), and there is no cached derived graph or incremental re-derivation.
- **Validated on Windows x64 with MSVC only.** The POSIX code paths for sockets,
  file I/O and the process helpers are implemented and compile, but they were not
  exercised on this host. See `VALIDATION.md`.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
