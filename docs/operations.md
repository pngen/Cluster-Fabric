# Cluster Fabric operations

This document covers building, installing, running and inspecting Cluster Fabric. Every flag
below is taken from the argument parser in the corresponding `apps/*.cpp` file.

## Build

Requirements: CMake 3.20 or newer and a C++20 compiler. The core library has no third-party
dependency (`CMakeLists.txt`, `NOTICE`). CUDA is optional.

```text
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Presets (`CMakePresets.json`):

| Preset | Generator | Binary directory | Notes |
| --- | --- | --- | --- |
| `release` | Ninja, `cl` | `build/release` | Release, tests, examples and benchmarks on. |
| `debug` | Ninja, `cl` | `build/debug` | Debug, tests, examples and benchmarks on. |
| `asan` | inherits `release` | `build/asan` | `CLUSTER_FABRIC_ENABLE_SANITIZERS=ON`, `CLUSTER_FABRIC_ENABLE_CUDA=OFF`. |
| `nocuda` | inherits `release` | `build/nocuda` | `CLUSTER_FABRIC_ENABLE_CUDA=OFF`. |

The presets pin `CMAKE_CXX_COMPILER` to `cl` (MSVC). On another toolchain, configure manually
instead of using the presets:

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Options:

| Option | Default | Effect |
| --- | --- | --- |
| `CLUSTER_FABRIC_BUILD_TESTS` | ON | Configures the test executables and `enable_testing()`. |
| `CLUSTER_FABRIC_BUILD_EXAMPLES` | ON | Builds the three example programs. |
| `CLUSTER_FABRIC_BUILD_BENCHMARKS` | ON | Builds `bench_cluster` and `bench_queries`. |
| `CLUSTER_FABRIC_ENABLE_CUDA` | ON | Includes `cmake/cuda_probe.cmake`, which builds the CUDA evidence probe if a toolkit is found. |
| `CLUSTER_FABRIC_ENABLE_SANITIZERS` | OFF | Adds address and undefined-behaviour sanitizers to every first-party target. |
| `CLUSTER_FABRIC_WARNINGS_AS_ERRORS` | ON | Adds `/WX` on MSVC and `-Werror` elsewhere. |
| `CLUSTER_FABRIC_CUDA_ARCHITECTURES` | `native` | CUDA architectures; CMake older than 3.24 cannot detect `native` and falls back to `75;80;90`. |

### CUDA on and off

- **CUDA off** (`-DCLUSTER_FABRIC_ENABLE_CUDA=OFF`, or no toolkit found):
  `find_package(CUDAToolkit QUIET)` fails, `cmake/cuda_probe.cmake` returns early, and
  `cluster_fabric_cuda` does not exist. `src/cuda_probe.cpp` provides the fallback definition
  of `run_cuda_probe`, which reports failure with reason `cuda_not_compiled`.
- **CUDA on**: `cluster_fabric_cuda` is built from `src/cuda/cuda_probe.cu` and
  `src/cuda_probe.cpp` with `CLUSTER_FABRIC_HAS_CUDA=1` so the fallback is preprocessed out
  (exactly one definition of `run_cuda_probe` exists in every build). The apps and tests that
  link it define `CLUSTER_FABRIC_APP_HAS_CUDA` / `CLUSTER_FABRIC_TEST_HAS_CUDA`. The core
  library never depends on CUDA.
- `--gpu` on the rack agent uses the probe; when it fails the agent prints why and publishes
  UNKNOWN device facts instead of inventing them (`apps/rack_agent_main.cpp:218`).

## Install and consume

```text
cmake --install build/release --prefix /opt/cluster-fabric
```

Installed artifacts (`CMakeLists.txt`):

- headers under `<prefix>/include/cluster_fabric` (every `*.hpp`);
- the library as `cluster_fabric` with `VERSION 1.0.0` and `SOVERSION 1`;
- the executables `cluster_fabric_coordinator`, `cluster_fabric_rack_agent` and
  `cluster_fabric_inspect` under `<prefix>/bin`;
- `cluster_fabric_cuda` when it was built;
- CMake package files in `<prefix>/lib/cmake/cluster_fabric`: `cluster_fabricTargets.cmake`
  (namespace `cluster_fabric::`), `cluster_fabricConfig.cmake` and
  `cluster_fabricConfigVersion.cmake` (`SameMajorVersion`).

Consuming:

```cmake
find_package(cluster_fabric 1.0 CONFIG REQUIRED)
add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE cluster_fabric::cluster_fabric)
# only when the CUDA probe was built and is wanted:
# target_link_libraries(my_service PRIVATE cluster_fabric::cluster_fabric_cuda)
```

`cluster_fabricConfig.cmake` calls `find_dependency(CUDAToolkit)` only when the build that
installed it had CUDA enabled, and `find_dependency(Threads)` on non-Windows platforms.

```cpp
#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

CoordinatorConfig config;
config.cluster = *ClusterId::parse("my-cluster");
config.port = 0;                       // ephemeral
config.persist_on_commit = false;

ClusterCoordinator coordinator(config);
const CoordinatorStartOutcome started = coordinator.start();
if (!started.ok) { /* started.status, started.error */ }
```

## Running the coordinator

```text
cluster_fabric_coordinator [options]
  --cluster <id>        cluster identity (default cluster-fabric-lab)
  --bind <address>      listener address (default 127.0.0.1)
  --port <n>            listener port, 0 selects an ephemeral port (default 0)
  --state <path>        durable state container path
  --no-persist          do not write durable state
  --min-racks <n>       readiness contract minimum current racks (default 1)
  --no-partial          readiness contract forbids PARTIAL
  --no-degraded         readiness contract forbids DEGRADED
  --quiet               print only the bound port and terminal lifecycle
  --help                print this text
```

Behaviour (`apps/coordinator_main.cpp`):

- A durable store is created only when `--state` is given and `--no-persist` is absent; with
  no path the coordinator runs without persistence and says `durable state : disabled`.
- `--min-racks 0` is refused with exit code 2; an invalid cluster identity is refused with exit
  code 2; a failed start returns exit code 3.
- The readiness contract starts from `ReadinessContract::permissive()` and is then modified by
  `--min-racks`, `--no-partial` and `--no-degraded`.
- If the cluster epoch is not yet known, the process declares the cluster itself
  (`MutationKind::DeclareCluster`, lifecycle `FORMING`, reason
  `coordinator_process_start`). A refusal of that declaration returns exit code 4.
- The process runs until `Ctrl+C`/console close; it never stops on a timer.

Non-quiet output:

```text
cluster_fabric_coordinator Cluster Fabric 1.0.0 (<build>)
cluster          : cluster-fabric-lab
listening        : 127.0.0.1:52344
durable state    : ./lab.state
recovered        : yes (racks 8 domains 6 links 12 fenced 8)
coordinator epoch: 3
note             : durable state loaded: format version 1
declared         : cluster epoch 1
...
stopped          : commits 41 accepted 33 rejected 8 sessions 12
```

With `--quiet` the only startup output is the bound port on its own line, and the terminal
summary is suppressed.

## Running a rack agent

```text
cluster_fabric_rack_agent [options]
  --cluster <id>        cluster identity (default cluster-fabric-lab)
  --coordinator <h:p>   coordinator endpoint, for example 127.0.0.1:50051
  --rack <id>           rack identity (default rack-000)
  --publisher <id>      durable publisher authority (default rack-publisher)
  --generation <n>      rack generation to publish (default 1)
  --ttl <ms>            evidence time-to-live (default 30000)
  --heartbeat <ms>      heartbeat interval (default 1000)
  --devices <n>         accelerator count in the synthetic composition (default 8)
  --device-memory <mb>  device memory per accelerator (default 32768)
  --gpu                 measure the local CUDA device and publish MEASURED evidence
  --supersede-after <n> advance the rack generation after n publications
  --publish-once        publish once and exit
  --help                print this text
```

Behaviour (`apps/rack_agent_main.cpp`):

- `--coordinator` is mandatory: the parser returns false unless a host and a non-zero port were
  parsed, so the agent exits with code 2 and prints usage without it.
- Exit codes: 2 for usage or an invalid cluster/rack/publisher identity, 3 when the connect or
  handshake fails, 4 when the first publication is refused.
- Without `--gpu` the composition is explicitly SYNTHETIC
  (`composition_label = "synthetic-rack-agent-composition"`, provenance `SYNTHETIC`) with
  `--devices` accelerators of `--device-memory` MiB each, plus fixed CPU, memory and NIC
  counts. With `--gpu` the composition comes from the measured device and the endpoint
  connectivity is `DirectFabric` instead of `Unknown`.
- The agent publishes once, optionally supersedes the rack generation, then heartbeats every
  `--heartbeat` ms until stopped. A refused heartbeat ends the loop with a diagnostic.
- Output includes the coordinator, cluster and topology epochs from the handshake, the accepted
  generation and membership generation, and a final line with the publication, heartbeat and state
  counts.

```text
connected        : coordinator epoch 3 cluster epoch 1 topology epoch 1
published        : generation 7 membership 3 lifecycle FORMING
stopped          : publications 1 heartbeats 42 state Stopped
```

## Running the inspector

```text
cluster_fabric_inspect [options]
  --file <path>            inspect a durable state container (read-only)
  --coordinator <host:port> inspect a live coordinator (read-only)
  --json                   emit machine-readable JSON
  --verbose                include per-rack and per-link detail
  --help                   print this text
```

- Exactly one of `--file` and `--coordinator` is required.
- The file path decodes the container, runs `validate_persisted_state` and prints the container
  status, the structural check and the recorded counters. A container that cannot be loaded exits
  with code 3.
- The live path uses only `HELLO`, `SNAPSHOT_REQUEST` and `VALIDATE_SNAPSHOT_REQUEST`; it
  never sends a mutation. Connection or handshake failures exit with code 4.
- The live path sends its HELLO with the cluster identity `cluster-fabric-inspect`
  (`apps/inspect_main.cpp:410`). The coordinator completes a handshake only for its own cluster
  identity, so live inspection matches a coordinator started with
  `--cluster cluster-fabric-inspect`; against any other cluster the handshake is refused with
  `REJECT_WRONG_CLUSTER`. This is a property of the current code, not a documented option.

Live output:

```text
cluster_fabric_inspect Cluster Fabric 1.0.0 (<build>)
source            : coordinator 127.0.0.1:52344
cluster           : cluster-fabric-inspect
coordinator epoch : 3
cluster epoch     : 1
generation        : 41
membership        : 3
topology          : epoch 1 generation 9
connectivity      : 7
health            : 12
lifecycle         : READY
racks             : 8
links             : 12
domains           : 14
semantic digest   : 3f1c8a2d5b7e9014
snapshot current  : yes
snapshot consumable: yes
```

Stale reasons are printed one per line as `stale reason      : <REASON> <subject>`, and
`--verbose` adds one line per rack binding with its generation, membership and boot identity.

## Reading readiness and blockers

`evaluate_readiness()` is deterministic and returns a `ReadinessEvaluation` whose
`blockers` list is sorted by (code, subject). Read it as: `satisfied` is true only when the
blocker list is empty, and `lifecycle` is derived from the blockers and the contract.

| Blocker code | Meaning |
| --- | --- |
| `cluster_retired`, `cluster_retiring` | The cluster is `RETIRED` or `RETIRING`; no other check is run. |
| `no_active_rack` | No rack has membership `ACTIVE`. |
| `minimum_current_racks_not_met` | Current active racks are below `minimum_current_racks`. |
| `mandatory_rack_missing`, `mandatory_rack_not_active`, `mandatory_rack_not_current` | A rack listed in `mandatory_racks` is absent, not active, or not authoritative current. |
| `active_rack_not_current` | `require_all_active_racks_current` is set and an active rack is not authoritative current. |
| `rack_revalidation_required` | An active rack needs revalidation. |
| `connectivity_evidence_missing`, `minimum_current_links_not_met` | `require_connectivity_evidence` is set and current links are absent or below `minimum_current_links`. |
| `placement_domain_class_missing`, `failure_domain_class_missing` | A required domain class has no current domain. |
| `structural_conflict` | Only when `require_no_conflicts` is set: a link references a rack that is not a member, a rack is authoritative current under a fenced process incarnation, or a retired rack is still authoritative current. |

Lifecycle outcomes, in the order the code decides them (`src/cluster_state.cpp:523`):

```text
RETIRED / RETIRING            cluster lifecycle
REVALIDATION_REQUIRED         a required element needs revalidation
READY                         no blockers
PARTIAL                       no blockers, knowingly incomplete, and allow_partial
DEGRADED                      blockers, but at least one rack is current
FORMING                       blockers, racks are active, none current
DECLARED                      blockers, no active rack
```

For a human-readable reason, use `explain_lifecycle()`, `explain_rack()`,
`explain_topology_currentness()`, `explain_snapshot_staleness()`,
`explain_failure_domain()` and `explain_reachability()`; each returns an `Explanation`
whose factors are sorted and bounded, and whose `describe()` output is deterministic. An
UNKNOWN answer always carries the reason it is UNKNOWN, for example "no current link carries
current evidence for this rack pair; UNKNOWN is not evidence of connectivity".

## Handling a coordinator restart

1. Restart with the same `--state` path. The coordinator loads the container and reports
   `recovered` counts and the new `coordinator epoch`.
2. Every recovered rack becomes non-authoritative and revalidation-required; its last boot
   identity is fenced with reason `coordinator_restart`. The lifecycle is recomputed and will be
   `REVALIDATION_REQUIRED` while any recovered rack remains unrevalidated.
3. Restart each rack agent. It generates a fresh boot identity, re-handshakes against the new
   coordinator epoch, and republishes; a publication from the old incarnation is refused with
   `REJECT_STALE_RACK_BOOT`.
4. Rack agents that stay connected across a restart see their heartbeats refused with
   `REJECT_STALE_COORDINATOR_EPOCH` and must reconnect.
5. Inspect the result with `cluster_fabric_inspect --file <state>` for the durable view and
   `cluster_fabric_inspect --coordinator <host:port>` for the live view. The file view prints
   the recorded coordinator epoch and notes that a live coordinator advances it on restart.
6. A restart with no `--state` starts from nothing: no racks, no links, no domains, and
   coordinator epoch 2. Durable declarations are the only thing that survives.

## Limits of the validated deployment

- The validated deployment is **a single host**: one coordinator process, one or more rack-agent
  processes, all on the same machine, communicating over **loopback TCP** (the default bind address
  is `127.0.0.1`) as **real, separate operating-system processes**. Process death, restart,
  fencing and stale replay are validated against that shape.
- **Multi-node composition is SYNTHETIC.** The synthetic laboratory
  (`include/cluster_fabric/synthetic.hpp`) generates a deterministic multi-rack scenario whose
  evidence is classified `SYNTHETIC` and which is "never a physical fact"
  (`include/cluster_fabric/evidence.hpp`). The rack agent's default composition is also
  synthetic unless `--gpu` succeeds.
- The transport is IPv4 TCP with no transport security: no TLS, no peer authentication, no
  authorization beyond the publisher/boot authority checks in the mutation pipeline. Bind to a
  trusted interface or wrap it; do not expose the listener to an untrusted network.
- One coordinator per cluster identity. There is no leader election, failover, replication or
  standby. The coordinator epoch exists to detect a restart, not to coordinate replicas.
- Persistence is a single file replaced atomically. There is no journal, no backup rotation and no
  replication; a lost file means a fresh start, and a corrupt file is rejected rather than
  partially loaded.
- The request queue is bounded (`kMaxPendingRequests`, 4096) and the session count is bounded
  (`kMaxSessions`, 1024); overload is reported as `REJECT_LIMIT_EXCEEDED` or a refused
  connection, never by dropping a mutation silently.
- `src/synthetic.cpp` is listed in `CLUSTER_FABRIC_SOURCES` but is absent from this checkout,
  so a build of this tree fails to link the synthetic laboratory and the examples and tests that
  use it. See [testing.md](testing.md).

Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
