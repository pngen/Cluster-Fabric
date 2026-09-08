# Cluster Fabric persistence

Durable state is a versioned, integrity-checked container. The format is defined by
`include/cluster_fabric/persistence.hpp` and implemented in `src/persistence.cpp`. Its
purpose is narrow: a restarted coordinator must remember what the infrastructure **declared**, and
must never mistake that memory for a current observation.

- Container magic: `kPersistenceMagic = 0x31534643`, the ASCII bytes `'C','F','S','1'` in
  little-endian order (`include/cluster_fabric/version.hpp`).
- Format version: `kPersistenceFormatVersion = 1`. `is_compatible_format()` accepts exactly
  that value.
- Maximum container size: `kMaxPersistenceBytes = 256 MiB`.

## Container header

The container begins with a 16-byte header (`kContainerHeaderBytes`, `src/persistence.cpp:64`).
Offsets are from the first byte of the file. Every integer is little-endian and is written with
`std::memcpy` from the value's in-memory representation, which is little-endian on every
supported target.

| Offset | Size | Field | Value |
| --- | --- | --- | --- |
| 0 | 4 | `magic` (u32) | `0x31534643`. |
| 4 | 2 | `format_version` (u16) | `1`. |
| 6 | 2 | `flags` (u16) | Reserved. Written as 0; any other value fails with `INVALID_STATE`. |
| 8 | 4 | `payload_length` (u32) | Declared payload size in bytes, not including the header. |
| 12 | 4 | `checksum` (u32) | See below. |
| 16 | `payload_length` | payload | Deterministic binary body, in the order below. |

### Checksum construction

```text
checksum = crc32(header[0,12)) XOR crc32(payload[0,payload_length))
```

- The header region covered is bytes `[0,12)`: magic, format version, flags and payload length.
  The checksum field itself is not covered.
- `crc32` is the same reflected CRC32 used by the wire protocol: polynomial `0xEDB88320`,
  initial value `0xFFFFFFFF`, final inversion (`src/protocol.cpp:425`).
- Encoding computes the two halves separately and stores their XOR
  (`src/persistence.cpp:752`); decoding recomputes both and compares
  (`src/persistence.cpp:832`). A mismatch is `CHECKSUM_MISMATCH`.

## Atomic replace strategy

`FilePersistenceStore::save()` (`src/persistence.cpp:2159`):

1. Encodes the state. An encode failure is returned unchanged (no file is touched).
2. Discards any leftover temporary file from a previous failed save.
3. Builds the temporary path as
   `<path>.tmp-<process_id>-<monotonic_counter>`, where the counter starts at 0 and increments on
   every save (`src/persistence.cpp:2176`). The name is unique per process and per attempt, and
   it is always in the destination directory, so the replace is a same-volume operation.
4. Writes the whole container to the temporary file, flushes, and checks the stream state. A
   failure removes the temporary file and reports `TEMP_FILE_UNAVAILABLE` or `IO_ERROR`.
5. Replaces the committed container atomically:
   - Windows: `MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`;
   - elsewhere: `std::rename(tmp, path)`.
   A failure removes the temporary file and reports `ATOMIC_REPLACE_FAILED`.
6. Clears the temporary path and reports `bytes_written`.

The destructor and `discard_temporary()` remove a leftover temporary file and never touch the
committed container. A crash between step 4 and step 5 leaves the previous committed container
intact and a stray `.tmp-` file that the next save removes.

Persistence is attempted once per commit: `persist_now()` maps a failed save to
`RejectionReason::PersistenceFailed` and the mutation is rolled back. `kMaxPersistenceRetries`
is declared in `include/cluster_fabric/limits.hpp` but no code path retries a save.

## Payload byte layout

`encode_persisted_state()` (`src/persistence.cpp:528`) writes the following fields in exactly
this order. "text" is a u32 length prefix followed by that many bytes; "optional" is a presence
byte (0 or 1) followed by the value only when present.

```text
 1  cluster id                                   text
 2  cluster epoch                                u64
 3  last coordinator epoch                       u64
 4  cluster generation                           u64
 5  membership generation                        u64
 6  topology epoch                               u64
 7  topology generation                          u64
 8  connectivity generation                      u64
 9  health generation                            u64
10  constraint generation                        u64
11  domain generations                           u64 x 8
     placement, capacity, failure, network, storage, power, cooling, link
12  snapshot generation                          u64
13  publication generation                       u64
14  lifecycle                                    u8
15  readiness contract:
      minimum_current_racks                      u64
      mandatory_racks                            u32 count + text each
      require_all_active_racks_current           bool
      require_connectivity_evidence              bool
      minimum_current_links                      u64
      required_placement_domain_classes          u32 count + u8 each
      required_failure_domain_classes            u32 count + u8 each
      allow_partial                              bool
      allow_degraded                             bool
      require_no_conflicts                       bool
16  topology record:
      epoch                                      u64
      generation                                 u64
      established_at                             i64 (unix millis)
      evidence stamp                             u8 provenance, i64 observed_at, u8 freshness, i64 ttl
      reason                                     text
17  declared_at                                  i64
18  last_mutation_at                             i64
19  racks                                        u32 count, then per rack, in RackId order:
      rack id                                    text
      rack generation                            u64
      membership                                 u8
      publisher                                  text
      last accepted publication                  u64
      last boot identity                         text
      composition:
        provenance                               u8
        accelerators                             u32 count, then per class:
          vendor                                 u8
          family                                 text
          device_count                           optional u32
          device_memory_bytes                    optional u64
        cpu_sockets                              optional u32
        cpu_cores                                optional u32
        host_memory_bytes                        optional u64
        nic_count                                optional u32
        switch_count                             optional u32
        composition_label                        text
      endpoints                                  u32 count, then per endpoint:
          endpoint id                            text
          network_domain                         optional text
          link_domain                            optional text
          connectivity                           u8
          evidence stamp                         u8, i64, u8, i64
      failure_domain_hints                       u32 count, then per hint:
          class                                  u8
          domain id                              text
          evidence stamp                         u8, i64, u8, i64
      rack lifecycle                             u8
      membership generation                      u64
      origin label                               text
      declared_at                                i64
20  placement domains                            u32 count, then per domain, in id order:
      id, class u8, member racks (u32 count + text each), domain header
21  capacity domains                             u32 count, then per domain:
      id, class u8, member racks, quantities (u32 count, then per quantity:
        unit text, optional f64 value, provenance u8, aggregated bool), domain header
22  failure domains                              u32 count, then id, class u8, members, header
23  network domains                              u32 count, then id, connectivity u8, members, header
24  storage domains                              u32 count, then id, members, header
25  power domains                                u32 count, then id, members, optional parent text, header
26  cooling domains                              u32 count, then id, members, optional parent text, header
27  link domains                                 u32 count, then id, members, header
28  links                                        u32 count, then per link, in InterRackLinkId order:
      id, source, destination                    text
      direction                                  u8
      connectivity                               u8
      network_domain                             optional text
      link_domain                                optional text
      source_endpoint                            optional text
      destination_endpoint                       optional text
      bandwidth_class                            u8
      nominal_bandwidth_bps                      optional u64
      latency_class                              u8
      nominal_latency_nanos                      optional u64
      hop_count                                  optional u32
      reachability                               u8
      health                                     u8
      failure_domains                            u32 count + text each
      domain header                              (evidence stamp, generation u64,
                                                  optional publisher text,
                                                  cluster epoch u64, coordinator epoch u64,
                                                  label text)
      topology_epoch                             u64
      topology_generation                        u64
      publication                                u64
      boot                                       optional text
29  constraints                                  u32 count, then per constraint, in id order:
      id, kind u8, racks (u32 count + text each), domain_ref text, statement text, domain header
30  retired racks                                u32 count, then per entry, in rack order:
      rack id text, last_generation u64, retired_at i64, reason text
31  withdrawn racks                              u32 count + text each
32  fenced authorities                           u32 count, then per entry, in boot order:
      boot text, rack text, reason text, fenced_at i64
```

The domain header is written by `write_header()` (`src/persistence.cpp:106`) and the evidence
stamp by `write_stamp()` (`src/persistence.cpp:75`), so every domain, link and constraint
carries provenance, observation timestamp, freshness and time-to-live.

## What is not persisted

`PersistedState` is a deliberately narrow projection of canonical state. The following are
**not** durable and are reconstructed as unknown or revalidation-required on restart:

- the live authority of a rack: `authoritative_current`, `RackCurrentness`,
  `non_authoritative_reason` and the rack's `HealthState` are not stored;
- membership evidence stamps (only the membership state and its generation are stored);
- derived indexes (`ClusterIndexes`), snapshots, the semantic digest, statistics counters and
  the request queue;
- the currentness of recovered evidence: even though evidence stamps are stored, recovery
  overwrites rack provenance with `RECONSTRUCTED` and freshness with
  `REVALIDATION_REQUIRED`, and sets link reachability and health to `Unknown`;
- the boot identity is stored as `last_boot` only so that recovery can fence it; it is never
  restored as live authority;
- anything about the running process: coordinator start time, session state, socket handles.

The rule is the one in the header: "Live process authority, reachability, health and currentness
are never restored as current: they are cleared and require revalidation."

## Decode validation

`decode_persisted_state()` (`src/persistence.cpp:762`) validates in this order and returns on
the first failure. Every failure carries a `StructuredError` with category `Persistence`,
stage `Decode` (or `Recover` when raised by a store) and a reason code equal to the lower-case
status name.

| Status | Raised when |
| --- | --- |
| `EMPTY_INPUT` | The input is zero bytes. |
| `TRUNCATED_HEADER` | Fewer than 16 bytes are available. |
| `BAD_MAGIC` | The first four bytes are not `0x31534643`. |
| `UNSUPPORTED_VERSION` | The format version is not `1`. |
| `INVALID_STATE` | Reserved header flags are non-zero; also used when a field fails a structural rule that is not a more specific status. |
| `PAYLOAD_TOO_LARGE` | The declared payload exceeds `kMaxPersistenceBytes` (or the file exceeds it while loading). |
| `TRUNCATED_BODY` | The declared payload length exceeds the bytes available after the header. |
| `TRAILING_GARBAGE` | Bytes remain after the declared payload, or unread bytes remain after the last record. |
| `CHECKSUM_MISMATCH` | `crc32(header[0,12)) XOR crc32(payload)` differs from the stored checksum. |
| `ABSURD_COUNT` | A record count exceeds its bound (`kMaxRacksPerCluster`, `kMaxInterRackLinks`, `kMaxDomainsPerClass`, `kMaxConstraints`, `kMaxDomainMembers`, `kMaxCapacityQuantities`, `kMaxRackEndpoints`, `kMaxFailureDomainRefs`, `kMaxDecodedRecords`). |
| `INVALID_ENUM` | A decoded enum value is outside its declared range (provenance, freshness, lifecycle, membership, rack lifecycle, connectivity, direction, reachability, health, vendor, domain class, constraint kind). |
| `INVALID_IDENTITY` | A decoded identity is malformed, missing or fails `validate_identity`; also raised for a missing cluster identity. |
| `BOUNDS_EXCEEDED` | The byte writer refused a field because a bound in `limits.hpp` was exceeded during encoding. |
| `DUPLICATE_RACK_IDENTITY` | Two durable rack entries share an identity. |
| `DANGLING_REFERENCE` | A domain, link or constraint references a rack or domain that is not present. |
| `INVALID_EPOCH` | The cluster epoch, coordinator epoch or recorded topology epoch is zero. |
| `INVALID_GENERATION` | A top-level generation, a rack generation or a domain generation is zero or inconsistent. |
| `INVALID_STATE` | Structural rules that have no more specific status: unsorted or duplicated lists, a link endpoint that is not a member, a direction of UNKNOWN, a self loop, and similar. |
| `IO_ERROR` | The store could not open, read or write the file, or no path was configured. |
| `ATOMIC_REPLACE_FAILED` | The temporary container could not replace the committed container. |
| `TEMP_FILE_UNAVAILABLE` | The temporary container could not be opened for writing. |
| `MISSING_FILE` | No container exists yet. This is reported by a store's `load()`, not by the decoder, and is treated as a fresh start. |

`decode_persisted_state` never returns a partially valid state: it either yields
`PersistenceStatus::Ok` with a complete `PersistedState` or a non-OK status with no state.
`validate_persisted_state()` (`src/persistence.cpp:1806`) is the structural validation applied
after decoding and before any commit; it is also callable directly.

## What recovery does to recovered state

`ClusterCoordinator::Impl::load_durable_state()` (`src/coordinator.cpp:77`) and
`from_persisted_state()` (`src/coordinator_engine.cpp:203`) apply the following transformation.
The full recovery sequence is described in [architecture.md](architecture.md).

| Recovered field | Result after restart |
| --- | --- |
| Cluster identity, cluster epoch, all generations, lifecycle, readiness contract, topology record, declared and last-mutation timestamps | Restored as recorded. |
| Rack identity, rack generation, membership state, membership generation, publisher, last accepted publication, composition, endpoints, failure-domain hints, rack lifecycle, origin label, declared_at | Restored as recorded. |
| Rack evidence | Provenance `RECONSTRUCTED`, freshness `REVALIDATION_REQUIRED`, ttl 0, observed_at = the recorded declared_at. |
| Rack currentness | `RevalidationRequired`. |
| Rack `authoritative_current` | false, with reason `recovered_state_requires_revalidation`. |
| Rack health | `Unknown`. |
| Rack boot identity | Added to `fenced_authorities` with reason `coordinator_restart`. |
| Links | Restored, but reachability and health are `Unknown` and evidence is revalidation-required. |
| Domains and constraints | Restored with evidence refreshed against the current clock, so a time-to-live that has elapsed becomes `STALE`. |
| Retired racks, withdrawn racks, fenced authorities | Restored; they are declarations, not observations. |
| Coordinator epoch | The recorded `last_coordinator_epoch` plus one. |
| Lifecycle | Recomputed by `evaluate_readiness()` over the recovered state. |
| `RecoveryReport` | Counts, previous and current coordinator epoch, whether the epoch advanced, and whether revalidation is required; notes are sorted. |

A recovered rack therefore stays in the cluster as a member with a known generation, but it is
not current and not authoritative until its publisher republishes under a fresh boot identity
(`MutationKind::RevalidateRecoveredState` or a normal publication).

Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
