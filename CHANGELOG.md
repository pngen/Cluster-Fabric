# Changelog

All notable changes to Cluster Fabric are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project adheres to
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-09-08

First release. Cluster Fabric 1.0.0 is the initial stable public API: the headers under
`include/cluster_fabric/` are frozen for the 1.x series.

### Added

* **Cluster identity and lifecycle.** Declaration, retirement, lifecycle states, and a
  readiness contract with typed blockers (`mandatory_rack_missing`,
  `mandatory_rack_not_current`, `minimum_current_racks_not_met`, `no_active_rack`, and
  others), sorted by code and subject.
* **Rack membership and authority.** Publisher registration, admission, membership
  transitions, rack generations, publication monotonicity, boot identity, fencing, and
  retirement with a durable retired-identity record.
* **Topology.** Inter-rack links with direction, endpoints, locality and bandwidth/latency
  classes, hop count, failure-domain and network/link-domain references; topology epochs,
  supersession, and explicit revalidation of links observed under a superseded epoch.
* **Domains.** Placement, capacity, failure, network, storage, power (with parent
  hierarchy), and cooling (with parent hierarchy) domains, each with class-specific
  validation, generation-keyed supersession, and aggregate domain generations.
* **Constraints and health.** Constraint records with kinds and scope, connectivity and
  health currentness publication, and evidence withdrawal with typed consequences.
* **Snapshots.** Immutable snapshots with an FNV-1a semantic digest, generation bindings,
  typed staleness reasons, and separate `current` and `consumable` verdicts.
* **Persistence.** A CRC32-protected, versioned, atomic container with a 256 MiB bound,
  truncation/corruption rejection with typed statuses, and recovery that clears dynamic
  observation, fences the recovered boot, and advances the coordinator epoch.
* **Protocol.** A framed TCP protocol with a 28-byte little-endian header, CRC32 over
  header and payload, version tag in every payload, allocation bounds enforced before
  reading, and adversarial rejection of bad magic, unknown types, absurd counts, invalid
  identities, and single-bit flips.
* **Tools.** `cluster_fabric_coordinator`, `cluster_fabric_rack_agent` (a real process
  with boot identity, heartbeat, supersession, and revalidation), and
  `cluster_fabric_inspect` (read-only, `--file` or `--coordinator`, JSON output).
* **Optional CUDA evidence.** `cluster_fabric_cuda` produces `MEASURED` composition
  evidence from a real device; the core library builds and installs without CUDA.
* **Synthetic scenario generator.** Seeded, deterministic cluster generation for
  mechanics, invariants, and determinism proofs. Synthetic evidence is never consumable.
* **Tests, examples, and benchmarks.** A zero-dependency test harness, three examples, two
  benchmarks measuring completed operations, and property tests with recorded seeds.

### Notes

* Cluster Fabric is a cluster-level authority and coordination layer. It does not
  schedule, place, broker, reserve, or drive fabrics; see `docs/boundaries.md`.
* The verified platform is a single Windows host with one RTX 5090. Loopback TCP proves
  real multiprocess and protocol behavior on one host; multi-rack compositions in this
  repository are synthetic and labelled as such.
* No telemetry, analytics, or crash uploads of any kind.
