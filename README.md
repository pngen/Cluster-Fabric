# Cluster Fabric

Cluster Fabric is a vendor-neutral, cluster-level infrastructure runtime. It answers one
question for everything above it in the stack, and answers it with evidence it can defend:

> What racks and cluster-level infrastructure constitute this cluster **now**, how are those
> racks related and connected, which topology and failure domains are authoritative, and
> which cluster generation may safely be consumed by higher-level runtimes?

It is a **coordination and authority** layer, not a scheduler, a fabric driver, or a
collector. It records what is authoritative, when it was observed, who published it, and
whether it may still be trusted. It never invents a positive answer: **UNKNOWN is a
first-class value and never silently becomes a positive one.**

```
   higher-level runtimes (schedulers, capacity, reservation, communication planners)
        |   read-only snapshots, typed staleness, explicit consumability
        v
   +-----------------------------------------------------------------------+
   |  Cluster Fabric  --  rack membership, inter-rack connectivity,        |
   |  cluster topology, placement / capacity / failure domains, locality   |
   |  classes, topology epochs, rack generations, reachability, hierarchy, |
   |  cluster lifecycle and readiness                                       |
   +-----------------------------------------------------------------------+
        ^                                        ^
        | framed TCP, authority-checked          | framed TCP, authority-checked
   rack agents (real processes)             the CUDA evidence probe (optional)
```

## What it is not

Cluster Fabric deliberately does **not** absorb the responsibilities of: Rack Fabric,
Fabric Scheduler, Communication Planner, Collective Scheduler or Collective Fabric,
Resource Broker, Capacity Fabric, Reservation Fabric, Congestion Fabric, any
NVLink/NVSwitch/GPU-Direct/RDMA-buffer/DPU/CXL/PCIe/storage fabric, Heterogeneous
Accelerator Federation, or Cross-Cluster State Fabric. It publishes cluster-level truth
those systems consume. See `docs/boundaries.md`.

## Requirements

* A C++20 compiler. Verified with MSVC 19.44 (Visual Studio 2022) and GCC/Clang-class
  toolchains.
* CMake 3.24 or newer.
* Windows or Linux. The verified platform is Windows 11 x64 with Windows SDK 10.0.26100.
* Optional: a CUDA toolkit (12.9 or 13.1 verified) and an NVIDIA GPU for the
  `cluster_fabric_cuda` evidence probe. The core library builds, installs, and runs
  without CUDA.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

CMake options (all default to `ON` except sanitizers):

| Option | Effect |
| --- | --- |
| `CLUSTER_FABRIC_BUILD_TESTS` | Build the test suite (no external test dependency). |
| `CLUSTER_FABRIC_BUILD_EXAMPLES` | Build the three runnable examples. |
| `CLUSTER_FABRIC_BUILD_BENCHMARKS` | Build the two benchmarks. |
| `CLUSTER_FABRIC_ENABLE_CUDA` | Build `cluster_fabric_cuda` when a toolkit is found. |
| `CLUSTER_FABRIC_ENABLE_SANITIZERS` | Add address/UB sanitizers where supported. |
| `CLUSTER_FABRIC_WARNINGS_AS_ERRORS` | Treat first-party warnings as errors (`/W4 /WX`). |

There is no network access at configure or build time and no vendored third-party code.

## Install and consume

```sh
cmake --install build --prefix /your/prefix
```

```cmake
find_package(cluster_fabric 1.0.0 REQUIRED)
target_link_libraries(your_target PRIVATE cluster_fabric::cluster_fabric)
# optional, only if you call run_cuda_probe():
target_link_libraries(your_target PRIVATE cluster_fabric::cluster_fabric_cuda)
```

The installed package exports `cluster_fabric::cluster_fabric` and, when built with CUDA,
`cluster_fabric::cluster_fabric_cuda`. The examples directory contains a complete
consumer that lives outside the source tree.

## Quick start: embed a coordinator

```cpp
#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

MemoryPersistenceStore store;                 // or FilePersistenceStore("/var/lib/cf.state")
ClusterCoordinator coordinator{CoordinatorConfig{}, &store};

MutationRequest declare;
declare.kind = MutationKind::DeclareCluster;
declare.cluster = *ClusterId::parse("prod-east");
declare.authority.cluster_epoch = ClusterEpoch::from_raw(1);
declare.declared_lifecycle = ClusterLifecycle::Declared;
declare.readiness_contract = ReadinessContract::permissive();
declare.evidence = EvidenceStamp::make(EvidenceProvenance::Reported, default_clock().now(), 30'000);
const MutationResult declared = coordinator.submit(declare);
if (!declared.accepted()) { /* declared.reason and declared.error explain exactly why */ }
```

Every mutation is accepted, refused, or reported as `NoChange`/`RevalidationRequired`
with a typed `RejectionReason`, a structured error carrying the stage and subject, and a
bounded `Explanation`. Nothing is applied partially: the coordinator validates authority,
then generation, then references, applies under an undo journal, verifies invariants,
persists atomically, and only then publishes the new state.

## Quick start: consume a snapshot

```cpp
const ClusterSnapshot snapshot = coordinator.snapshot();
const SnapshotValidation validation = snapshot.validate(coordinator.state_copy());
if (!validation.current) {
  // Typed, sorted reasons such as ClusterGenerationAdvanced or RackGenerationSuperseded,
  // each with the subject that caused it.
}
if (validation.consumable) {
  // Safe to consume as the current composition.
}
```

`current` means every generation the snapshot was bound to still matches authority.
`consumable` additionally requires that the cluster is in a lifecycle a consumer may act
on. Fabricated (`SYNTHETIC`), `ESTIMATED`, `RECONSTRUCTED`, and `UNKNOWN` evidence can
never satisfy a current-evidence requirement; only `MEASURED` and `REPORTED` can. A
cluster built entirely from synthetic evidence is therefore deliberately never consumable:
it exercises mechanics, invariants, and determinism, and it says so.

## Running a real cluster

```sh
# Coordinator: framed TCP, durable state, readiness contract.
cluster_fabric_coordinator --cluster prod-east --bind 0.0.0.0 --port 9400 \
    --state /var/lib/cluster-fabric/prod-east.state --min-racks 2

# One real rack agent process per rack.
cluster_fabric_rack_agent --cluster prod-east --coordinator 127.0.0.1:9400 \
    --rack rack-01 --publisher rack-agent-01 --generation 1 --ttl 60000 --heartbeat 10000

# Read-only inspection of either a durable container or a live coordinator.
cluster_fabric_inspect --coordinator 127.0.0.1:9400 --json --verbose
cluster_fabric_inspect --file /var/lib/cluster-fabric/prod-east.state
```

`cluster_fabric_inspect` never mutates anything: it opens frames, reads, and exits. It
returns 0 on success, 2 on a usage error, 3 on a corrupt container, and 4 on a transport
failure.

The rack agent is a real OS process with its own boot identity. When it dies, the
coordinator fences that incarnation by boot id, marks everything it published as requiring
revalidation, and refuses replays from the fenced boot. A fresh process publishes under a
new boot identity and becomes authoritative again.

## Protocol and persistence

* Framed TCP: a 28-byte little-endian header (magic `0x31465043`, version, type, flags,
  payload length, CRC32) followed by a payload that starts with a `u16` protocol version.
  CRC32 is computed over bytes `[0, 24)` XOR the payload. Frames are bounded to 1 MiB with
  an absolute 16 MiB limit, and every count and length is checked before allocation.
* Durable containers: a 16-byte header (`0x31534643`, format version, flags, payload
  length, CRC32) followed by a canonical payload. Writes are atomic
  (`MoveFileExA` with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` through a
  `<path>.tmp-<pid>-<counter>` temporary) and bounded to 256 MiB. Truncation at any byte,
  single-bit flips, absurd counts, invalid identities, and unsupported versions are all
  rejected with a typed status and leave no partial state.
* Restart: a restarted coordinator loads durable state, clears every dynamic observation,
  marks racks non-authoritative and revalidation-required, frees links to `UNKNOWN`, fences
  the recovered boot with reason `coordinator_restart`, and advances `CoordinatorEpoch`.
  Stale replays from before the restart are refused by epoch.

See `docs/protocol.md` and `docs/persistence.md`.

## Invariants

The coordinator verifies a fixed invariant set after every candidate application, before
persisting, and refuses any mutation that would violate one. The full list with rationale
is in `docs/invariants.md`; it covers single cluster identity, known epochs and
generations, monotonic generations, topology epoch consistency (including links observed
under a superseded epoch, which may only persist as `UNKNOWN` and marked
revalidation-required), rack membership consistency, reference integrity for domains,
links and constraints, domain member and capacity ordering, and fence/retirement
bookkeeping.

## Evidence and currentness

Every record carries provenance (`MEASURED`, `REPORTED`, `SYNTHETIC`, `ESTIMATED`,
`RECONSTRUCTED`, `UNKNOWN`), an observation timestamp, and a freshness bound. A record is
current only when its evidence is current and its authority still owns it. The optional
CUDA probe produces `MEASURED` composition evidence from a real device
(`examples/03_cuda_rack_evidence.cpp` runs a vector add, checks parity, restores the
device, and only then reports).

## Benchmarks

`bench_cluster` measures completed coordinator operations (add rack, publish link,
publish placement domain, capture snapshot, validate snapshot, evaluate readiness, check
invariants) at 10, 100, and 1000 racks. `bench_queries` measures read-side queries
(lookup, failure-domain independence, reachability, aggregate capacity, lifecycle
explanation, readiness) on a published capacity domain. Both report throughput only after
the operation has actually completed and been verified.

## Documentation

| Document | Contents |
| --- | --- |
| `docs/architecture.md` | Components, ownership, threading, and data flow. |
| `docs/boundaries.md` | What Cluster Fabric owns and what it deliberately refuses. |
| `docs/invariants.md` | Every invariant, why it exists, and how it is enforced. |
| `docs/protocol.md` | Frame format, message catalog, and adversarial rules. |
| `docs/persistence.md` | Container format, atomicity, corruption handling, recovery. |
| `docs/operations.md` | Running, upgrading, fencing, recovery, and troubleshooting. |
| `docs/testing.md` | Test areas, what each proves, and how to reproduce proofs. |
| `SECURITY.md` | Threat model and how to report a vulnerability. |
| `CHANGELOG.md` | Release history. |

## Platform honesty

The verified hardware is a single Windows workstation with one RTX 5090. Loopback TCP
proves real multiprocess, real framed-protocol, real process-death and real
persistence-recovery behavior **on one host**. Multi-rack and multi-node compositions in
this repository are synthetic and are labelled `SYNTHETIC`; they are never presented as
measured infrastructure. No test decides success by timing out: every proof either reaches
its condition or reports the condition it failed to reach.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
