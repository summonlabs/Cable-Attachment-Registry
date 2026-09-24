# Validation report

This file records what was actually executed and measured for Cable Attachment
Registry 1.0.0. Nothing here is projected or estimated.

## Environment

| Item | Value |
| --- | --- |
| Host | Windows 11 (x64), 16 logical processors, 61.6 GiB RAM |
| Compiler | MSVC 19.44.35222.0, toolset 14.44.35207 |
| Windows SDK | 10.0.26100.0 |
| Generator | Ninja 1.13.2 |
| CMake | 4.3.2 |
| Git | 2.52.0.windows.1 |
| Language | C++20, /W4 /permissive- /Zc:__cplusplus /utf-8 /WX |

No test in this repository sets a CTest timeout, a shell timeout wrapper, a
watchdog, or a forced termination that is classified as a pass. Every suite runs
to natural completion.

## Configurations built

| Configuration | Command | Result |
| --- | --- | --- |
| Debug | `cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug` then `cmake --build build-debug` | Warning-clean, no errors |
| Release | `cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release` then `cmake --build build-release` | Warning-clean, no errors |
| AddressSanitizer | `-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCABLE_ATTACHMENT_REGISTRY_SANITIZERS=ON` | Warning-clean, links against the MSVC ASan runtime, no reports |

The library, the two tools, the three examples, the benchmark and all eleven
test executables build in each configuration.

## Test results

`ctest --output-on-failure` run from each build tree.

| Build | Suites | Result |
| --- | --- | --- |
| Debug | 11 | 100% passed, 0 failed (5.90 s total) |
| Release | 11 | 100% passed, 0 failed (5.27 s total) |
| RelWithDebInfo with /fsanitize=address | 10 (the install and package-consumer test is excluded from sanitizer builds) | 100% passed, 0 failed, zero AddressSanitizer reports (3.50 s total) |

The Debug tree, case by case:

```
Test #1: cable-registry-test-adversarial ....... Passed
Test #2: cable-registry-test-authority ......... Passed
Test #3: cable-registry-test-concurrency ....... Passed
Test #4: cable-registry-test-foundation ........ Passed
Test #5: cable-registry-test-ipc ............... Passed
Test #6: cable-registry-test-multiprocess ...... Passed
Test #7: cable-registry-test-persistence ....... Passed
Test #8: cable-registry-test-property .......... Passed
Test #9: cable-registry-test-registry .......... Passed
Test #10: cable-registry-test-snapshot ......... Passed
Test #11: cable-registry-test-package-consumer . Passed
100% tests passed, 0 tests failed out of 11
```

## Multiprocess proof

`cable-registry-test-multiprocess` starts real executables and asserts the
following, each verified by the suite above:

- two publisher processes running at the same time against one daemon reach the
  same canonical graph digest as the same two publishers running one after the
  other;
- a query issued from a third process observes the published topology, the
  Attached state, and a typed NotFound failure for an unregistered object;
- a daemon terminated with TerminateProcess, with no destructor and no flush,
  leaves every acknowledged record durable: the restarted daemon reports the same
  digest, marks the recovered claims Unvalidated, and answers Unknown for the
  same port under --live-only;
- a restarted publisher that presents a higher incarnation fences its previous
  incarnation: the port becomes Unknown until the claim is re-asserted, after
  which it is Attached and Validated, and the old incarnation is refused with
  refused-fenced;
- a store with a bit flipped in the middle is refused by a fresh daemon process,
  which exits non-zero without ever reporting readiness, and opens again under
  --salvage with the damage reported.

## Install and downstream proof

```
cmake --install build-release --prefix <prefix>
cmake -S tests/package_consumer -B <consumer-build> "-DCMAKE_PREFIX_PATH=<prefix>" -DCMAKE_BUILD_TYPE=Release
cmake --build <consumer-build> --config Release
<consumer-build>/Release/consumer.exe
```

The installed tree contains 22 public headers, the static library, and the
exported CMake package under lib/cmake/CableAttachmentRegistry. The consumer
project is a separate CMake project: it never sees this repository's source tree
or build tree, it links SummonSoftwareLabs::CableAttachmentRegistry, and it
exercises registration, ingestion, the authoritative query, snapshot encoding and
decoding, and the loopback transport. It printed:

```
consumer ok: Attached on port 2, graph digest 241a9540d5e580bf0a59fd23cbb67f3a485c05b00679b0484e4147d90521456a
```

The same install, configure, build and run cycle is an automated CTest case
(`cable-registry-test-package-consumer`) that runs against the freshly built tree
in the Debug and Release configurations.

## Benchmarks

`cable-registry-benchmarks` measures completed useful work: records the registry
accepted (and, in the durable row, flushed to stable storage) before returning,
port answers fully derived and returned, and canonical snapshots actually built
and hashed. It measures no submission latency and no queue depth.

Workload: 4000 objects, 16 endpoints, 64 ports per endpoint, 8016 evidence
records (2016 registrations and 4000 attachments), single process, synthetic
data. Numbers are from one run on the host above; they are not a hardware claim.

| Measurement | Debug | Release |
| --- | --- | --- |
| Ingest accepted and applied (in memory) | 73 459 records/s | 1 383 882 records/s |
| Port queries answered | 45 767 queries/s | 1 160 261 queries/s |
| Canonical snapshot built and hashed | 4 snapshots/s | 63 snapshots/s |
| Ingest accepted and flushed to stable storage (one fsync per record) | 1 091 records/s | 1 168 records/s |
| Log compacted to one state image | 1 rewrite in 0.83 s | 1 rewrite in 0.07 s |

The durable row is bounded by the platform's flush-to-stable-storage cost, not by
the runtime: it is the same order in both configurations because every accepted
record performs an fsync.

## Defects found and fixed during validation

Every defect below was found by a test or a benchmark in this repository and
fixed before the release commit.

| Defect | How it was found | Fix |
| --- | --- | --- |
| 128 bit identifiers rendered only the first 16 hexadecimal digits | `test_foundation` identity round trip | The formatter now emits all 32 digits |
| Timestamps before the Unix epoch rendered with the wrong date | `test_foundation` RFC 3339 case | Floor division in the civil-date conversion |
| A receiver blocked in recv was not woken by shutdown on Windows | `test_ipc` shutdown case hung with no timeout to hide it | Connection threads wait for readiness in 25 ms slices and observe a stop flag; nothing is cancelled from another thread |
| A blocked accept was not cancelled by shutdown on Windows | `test_ipc` shutdown case | The acceptor waits on the listener and a dedicated wakeup socket, and is woken through it |
| A server with fewer workers than persistent connections could never serve the extra connections | `test_ipc` concurrent-clients case hung | One thread per connection, bounded by max_connections; a connection beyond the bound is refused rather than queued |
| The canonical snapshot digest depended on the registry lifetime through a validation counter | `test_persistence` reopen case | Lifetime-dependent counters are zero in the canonical image and live in the envelope |
| A superseded record did not always reach its claim key's bounded history ring, so the ring depended on arrival order | `test_snapshot` permutation case | Every claim key resolves independently and a losing record always feeds the ring |
| Compaction could not replace the log on Windows while the process held it open | `test_persistence` compaction case | The handle is released before the atomic rename, and the store is reopened after it |
| The encoder reserved exactly the bytes of the next write, making large encodings quadratic | Benchmarks: snapshot throughput collapsed from 8/s to 0.1/s between 500 and 4000 objects | Geometric growth up to the 8 MiB ceiling |
| Registering n objects scanned all n registrations to detect label reuse | Benchmarks: ingest throughput fell as the object count grew | A label index that is rebuilt when the state is replaced and updated as registrations are applied; a hit is always re-checked against the object's own registrations |
| Fenced-claim counting ran one pass over the whole state per object | Benchmarks: snapshot construction was quadratic | One pass over the records, grouped by object |

## Genuine limitations

- Validation was performed on Windows x64 with MSVC only. The POSIX socket, file
  I/O and process-helper paths are implemented and compile, but were not
  exercised on this host. No claim is made about them beyond that.
- The repository has no multi-host, distributed or consensus behaviour, and none
  is claimed. Every process test runs on one host over loopback.
- Authority is an operator declaration. The registry orders claims by it and has
  no way to check that the assignment reflects reality.
- Query and snapshot work is O(state). There is no cached derived graph and no
  incremental re-derivation.
