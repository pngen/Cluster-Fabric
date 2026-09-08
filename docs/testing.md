# Cluster Fabric testing

## The harness

The test harness is `tests/harness.hpp`. It has no dependency beyond the C++20 standard library:
"the repository must build and test from a fresh clone with no network access and no third-party
test framework". A test binary is a normal executable with a `main()` that calls
`cf_test::run("<area>")`.

- **Self-registering cases.** `CF_TEST(name)` declares a function, creates a static
  `cf_test::Registrar` for it, and pushes a `TestCase{name, body}` into a process-wide
  registry (`cf_test::registry()`). No list of tests has to be maintained by hand.
- **Typed failures.** `CF_FAIL(message)` throws `cf_test::Failure` with
  `file:line: message`. `CF_EXPECT(condition)` and `CF_EXPECT_EQ(expected, actual)` throw
  with a rendered comparison. `text_of()` renders enums through `to_string`, strong counters
  through `str()`, strong identities through `value()`, streamable values through
  `operator<<`, and everything else as `<value>`.
- **Exit code.** `cf_test::run()` prints `[PASS] name` or `[FAIL] name: message` for each
  case, prints one summary line `<area>: N passed, M failed`, and returns 0 only when
  `failed == 0`. An unexpected exception is a failure, not a crash.
- **No timeout, no watchdog.** The header states it plainly: "There is no timeout, no watchdog and
  no test that passes by terminating." `tests/CMakeLists.txt` states the same rule from the other
  side: "No test declares a CTest TIMEOUT: a test that only passes because something killed it is
  not a passing test." There is no `set_tests_properties(... TIMEOUT ...)` and no
  `CTestTestfile` timeout anywhere in the repository; the only test properties set are
  `WORKING_DIRECTORY` and `LABELS`.
- **Registration in CMake.** Each declared area becomes its own executable and its own CTest test,
  labelled `cluster_fabric`, running in the test binary directory. Each area is compiled with
  the same strict warning set as the library, and with the absolute paths of the three executables
  injected as `CLUSTER_FABRIC_TEST_COORDINATOR_PATH`, `CLUSTER_FABRIC_TEST_RACK_AGENT_PATH` and
  `CLUSTER_FABRIC_TEST_INSPECT_PATH` so a test can start real processes.
- **Missing sources are skipped, not fatal.** `tests/CMakeLists.txt` wraps each area in
  `if(EXISTS ...)` so independent work on the suite does not block a build.

## Test areas

`tests/CMakeLists.txt` declares 15 areas. Exactly one of them - `test_identity_generation` - has
a source file in this checkout; the other 14 are skipped at configure time and therefore register
no CTest test. The table below records what is declared and what is present, so the state of the
suite is unambiguous.

| Declared area | Source present | Subject the area denotes |
| --- | --- | --- |
| `test_identity_generation` | yes | Identity validation and the parsing/rendering of counters, epochs and evidence classifications. |
| `test_lifecycle_readiness` | no | Readiness contract evaluation and lifecycle derivation. |
| `test_membership` | no | Rack membership transitions and the membership generation. |
| `test_topology_domains` | no | Links, domains, independence and reachability queries. |
| `test_snapshot_staleness` | no | Snapshot capture, digest stability and typed stale reasons. |
| `test_persistence` | no | Container encoding, decoding, corruption handling and atomic replace. |
| `test_protocol` | no | Frame codec, payload codecs, version tagging and bounds. |
| `test_mutations` | no | The commit pipeline: authority, generations, references, invariants, idempotence. |
| `test_fencing_restart` | no | Fencing, coordinator restart and recovery. |
| `test_rack_agent` | no | The rack-side publisher: handshake, registration, publication, supersede, revalidate, heartbeat. |
| `test_synthetic_property` | no | Properties of the deterministic synthetic laboratory. |
| `test_race_deterministic` | no | Deterministic behaviour under concurrent submission, using `CommitHook` as an observer. |
| `test_process_death` | no | Real process death of a rack agent against a real coordinator. |
| `test_cuda_probe` | no | The optional CUDA evidence probe. |
| `test_cli` | no | The three executables and their argument parsing and output. |

The only area that runs today, `test_identity_generation`, covers:

- textual identity validation: the accepted alphabet and separators, the 128-byte bound, the
  reserved `.` and `..` forms, and rejection of empty, over-long, space-containing and
  path-like input;
- default construction: an unset identity is not known, has an empty view and is unequal to a
  parsed identity;
- boot identities: `make_rack_agent_boot_id` returns two distinct, valid identities;
- generations: `known()`, `next()`, `precedes()`, and that `next()` at the maximum value
  returns no value instead of wrapping;
- counter parsing: `parse_counter_text` accepts digits, rejects zero unless allowed, and rejects
  empty, signed, spaced and non-numeric text;
- evidence: a measured stamp is fresh until its time-to-live elapses, then stale; refresh,
  `mark_revalidation_required()`, synthetic and default stamps are never current; and the
  currentness eligibility rule excludes `ESTIMATED` and `UNKNOWN` while including `REPORTED`;
- enum text: canonical spellings render, TitleCase spellings parse for lifecycle, provenance,
  freshness and durability, and an unknown spelling returns no value;
- lifecycle consumability: `REVALIDATION_REQUIRED` is not consumable and `DEGRADED` is.

## How to run the suite

```text
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Equivalent explicit commands:

```text
ctest --test-dir build/release -L cluster_fabric --output-on-failure
ctest --test-dir build/release -R test_identity_generation --output-on-failure
build/release/test_identity_generation
```

Running a test binary directly is the primary interface: it prints one line per case and a summary,
and its exit code is the test result. `--output-on-failure` is the preset default for the
`release`, `debug` and `asan` test presets.

The examples and benchmarks are built but not registered as tests:

- `examples/01_embedded_cluster.cpp` - starts a coordinator in-process, builds a synthetic
  eight-rack cluster through the public mutation API, reads it back through the snapshot contract
  and exits non-zero if the invariant report is not OK;
- `examples/02_consumer_contract.cpp` - the consumer contract: what a caller may rely on;
- `examples/03_cuda_rack_evidence.cpp` - measured accelerator evidence;
- `benchmarks/bench_cluster.cpp` and `benchmarks/bench_queries.cpp` - throughput and query
  benchmarks.

## How the process-death, restart, stale-replay and corruption proofs work

These four proofs are the reason the test areas `test_process_death`, `test_fencing_restart`,
`test_protocol` and `test_persistence` exist. Their sources are not present in this checkout,
so they are not executed today; what follows is the mechanism each proof uses, all of which is
implemented in the library and reachable through the public API.

### Process death

The CMake configuration injects the absolute path of `cluster_fabric_coordinator` and
`cluster_fabric_rack_agent` into every test area, so a test starts a **real** coordinator process
and a **real** rack-agent process - not a thread. Killing the agent process closes its TCP session,
which is exactly the path the coordinator handles:

1. `detach_session` notices that the dead incarnation owns live evidence and queues a fence
   request with reason `session_closed`.
2. `apply_fence` clears `authoritative_current` on the racks and links it owned, marks their
   evidence revalidation-required, appends a `FencedAuthority`, advances the cluster and health
   generations, and persists.
3. The observable results are: `RECOVERED`-style non-authoritative reasons on the affected rack,
   a non-empty fenced set, and a lifecycle that no longer counts the rack as current. A restart of
   the agent produces a new boot identity, and a request carrying the old one is refused with
   `REJECT_STALE_RACK_BOOT`.

`src/rack_agent.cpp` and `apps/rack_agent_main.cpp` document this as the intent of the design:
"Killing it is the real process-death proof".

### Coordinator restart

The proof uses a `FilePersistenceStore` on a temporary path, or a `MemoryPersistenceStore`
retained across the two coordinator objects:

1. Run a coordinator, declare the cluster, publish racks, stop it.
2. Construct a new coordinator with the same store and call `start()`.
3. Assert on `recovery()`: `loaded`, `racks_recovered`, `domains_recovered`,
   `links_recovered`, `fenced_recovered`, `previous_coordinator_epoch`,
   `current_coordinator_epoch`, `coordinator_epoch_advanced` and `revalidation_required`.
4. Assert on canonical state: every recovered rack has `authoritative_current == false`,
   currentness `RevalidationRequired` and reason `recovered_state_requires_revalidation`; the
   lifecycle is `REVALIDATION_REQUIRED`; a republished rack becomes current again.

### Stale replay

The proof captures a request and re-submits it against a state that has moved on. Each case has a
distinct typed outcome:

| Replay | Expected rejection |
| --- | --- |
| Same rack publication, same publication counter | `REJECT_STALE_PUBLICATION` |
| Publication from a boot identity that was fenced | `REJECT_STALE_RACK_BOOT` |
| Request carrying the previous coordinator epoch | `REJECT_STALE_COORDINATOR_EPOCH` |
| Request carrying a superseded cluster epoch | `REJECT_STALE_CLUSTER_EPOCH` |
| Request carrying a superseded rack generation | `REJECT_STALE_RACK_GENERATION` |
| Request carrying a superseded topology epoch | `REJECT_STALE_TOPOLOGY_EPOCH` |
| Snapshot view captured before a change | the matching `SnapshotStaleReason` from `ClusterSnapshot::validate` or `ClusterCoordinator::validate` |

Byte-identical republication under the same generation is not a replay: it is answered with
`MutationOutcome::NoChange` and reason `idempotent`.

### Corruption

The proof mutates container bytes and asserts the typed status, using
`decode_persisted_state()` directly or `MemoryPersistenceStore::set_raw_bytes()` followed by
`load()`:

| Mutation | Expected status |
| --- | --- |
| Truncate below 16 bytes | `TRUNCATED_HEADER` |
| Flip a magic byte | `BAD_MAGIC` |
| Change the format version | `UNSUPPORTED_VERSION` |
| Set a reserved flag | `INVALID_STATE` |
| Declare a payload longer than the content | `TRUNCATED_BODY` |
| Append a byte | `TRAILING_GARBAGE` |
| Flip a payload byte | `CHECKSUM_MISMATCH` |
| Raise a record count past its bound | `ABSURD_COUNT` |
| Write an out-of-range enum | `INVALID_ENUM` |
| Corrupt an identity | `INVALID_IDENTITY` |
| Remove a referenced rack | `DANGLING_REFERENCE` |
| Duplicate a rack entry | `DUPLICATE_RACK_IDENTITY` |

Every case must yield the status and no state: `decode_persisted_state` never returns a partially
valid `PersistedState`. The atomic-replace path is proven separately by the
`FilePersistenceStore` behaviour: a failed save removes its temporary file and leaves the
committed container untouched.

## What the absence of the missing sources means

Because `tests/CMakeLists.txt` skips a missing area rather than failing configuration, a build of
this tree succeeds with a single registered test. A green `ctest` run therefore proves only what
`test_identity_generation` covers. The proofs described above are supported by the library, not
demonstrated by this checkout, and `src/synthetic.cpp` is likewise absent, so the synthetic
laboratory used by `examples/01`, `examples/03` and `test_synthetic_property` has no
definition here.

Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
