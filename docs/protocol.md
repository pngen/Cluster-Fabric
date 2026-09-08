# Cluster Fabric wire protocol

The transport between a cluster coordinator and its rack-side publishers is a bounded,
length-checked, checksummed binary frame protocol. It is defined in
`include/cluster_fabric/protocol.hpp` and implemented in `src/protocol.cpp`. The protocol
version is `kProtocolVersion = 1` and the frame magic is `kProtocolMagic = 0x31465043`
(`include/cluster_fabric/version.hpp`).

Every integer in a frame and in every payload is little-endian. No field is ever written in
native byte order.

## Frame header

The header is exactly 28 bytes (`kFrameHeaderSize = 28`). Offsets are from the first byte of
the frame.

| Offset | Size | Field | Value |
| --- | --- | --- | --- |
| 0 | 4 | `magic` (u32) | `0x31465043`, which is the ASCII bytes `'C','P','F','1'` in little-endian order. |
| 4 | 2 | `version` (u16) | `1`. A frame whose version is not `kProtocolVersion` fails with `ProtocolStatus::UnsupportedVersion`. |
| 6 | 2 | `type` (u16) | `MessageType`. A value above `kMaxMessageTypeRaw` (16) fails with `ProtocolStatus::UnknownMessage`. |
| 8 | 4 | `flags` (u32) | Reserved. Written as 0 by every caller in this repository and parsed but not validated by `decode_frame`. |
| 12 | 8 | `sequence` (u64) | Sender-local monotonic frame counter. The coordinator starts at 0 per session and increments before every frame it sends. |
| 20 | 4 | `payload_length` (u32) | Declared payload size in bytes, not including the header. |
| 24 | 4 | `checksum` (u32) | CRC32 over header bytes `[0,24)` followed by the payload. |
| 28 | `payload_length` | `payload` | Message payload, starting with a u16 protocol-version tag. |

Fields are encoded in that order by `encode_frame` (`src/protocol.cpp:429`) and decoded in
that order by `decode_frame` (`src/protocol.cpp:458`).

### CRC32 coverage

- Algorithm: reflected CRC32 with polynomial `0xEDB88320`, initial value `0xFFFFFFFF` and a
  final inversion (`crc32` in `src/protocol.cpp:425`; the 256-entry table is generated at
  compile time by `make_crc32_table`).
- Coverage: header bytes `[0,24)` - that is magic, version, type, flags, sequence and
  `payload_length` - followed by exactly `payload_length` payload bytes. The `checksum` field
  itself is not covered, which is why it is written into the frame after the rest of the header
  (`src/protocol.cpp:454`).
- Verification recomputes the same two regions (`src/protocol.cpp:510`) and fails the frame with
  `ProtocolStatus::ChecksumMismatch` on any difference.

### Size limits and rejection order

`decode_frame` applies its checks in this order, and the first failure wins:

1. fewer than 28 bytes available -> `TruncatedHeader`;
2. header field read failure -> `TruncatedHeader`;
3. magic mismatch -> `BadMagic` ("frame magic does not match CPF1");
4. version mismatch -> `UnsupportedVersion`;
5. type above `kMaxMessageTypeRaw` -> `UnknownMessage`;
6. `payload_length` greater than the effective maximum -> `OversizedFrame`;
7. `payload_length` greater than the bytes actually available -> `TruncatedPayload`;
8. checksum mismatch -> `ChecksumMismatch`.

The effective maximum is the caller's `max_payload` clamped to
`kAbsoluteMaxFramePayloadBytes`. Defaults and caps from `include/cluster_fabric/limits.hpp`:

| Bound | Value |
| --- | --- |
| `kMaxFramePayloadBytes` (default for `encode_frame` and `decode_frame`) | 1 MiB |
| `kAbsoluteMaxFramePayloadBytes` (absolute ceiling) | 16 MiB |
| `kMaxEncodedStringBytes` (any single length-prefixed string) | 4096 |
| `kMaxSessionOutboundBytes` | 4 MiB |
| `kMaxSessions` | 1024 |

A frame that exceeds the configured maximum is rejected before any byte of the payload is read
or allocated. The coordinator's session loop clamps `CoordinatorConfig::max_frame_payload` to
the absolute ceiling and falls back to `kMaxFramePayloadBytes` when it is zero
(`src/coordinator_network.cpp:453`). When the header declares a payload that has not arrived yet,
the session reads exactly that many further bytes and re-decodes; if the declared length exceeds
the maximum it answers with an `Error` frame carrying reason code `oversized_frame` and closes
the session (`src/coordinator_network.cpp:471`).

`decode_frame` reports `consumed = 28 + payload_length` and does not itself reject bytes after
the declared payload: the input may legitimately hold more than one frame. Payload-level trailing
bytes are a different matter and are always rejected - see "Payload framing" below.

## Payload framing

Every public payload begins with a u16 protocol-version tag.

- Writers go through `encode_message` (`src/protocol.cpp:2996`), which calls
  `write_version_tag` first; readers go through `decode_message`
  (`src/protocol.cpp:3005`), which calls `read_version_tag` first. A tag that is not
  `kProtocolVersion` fails with `ProtocolStatus::UnsupportedVersion` on the field named
  `version`.
- The four codecs that are also used by persistence - `encode_mutation_request`,
  `encode_mutation_result`, `encode_snapshot_view` and `encode_structured_error` - write the
  same tag explicitly (`src/protocol.cpp:3218`, `3238`, `3258`, `3278`).
- A payload decode that does not consume every byte fails with
  `ProtocolStatus::TrailingGarbage` (`trailing_garbage()`, reported with the number of
  remaining bytes). A decoder that detects a semantic error after consuming bytes calls
  `ByteReader::mark_invalid()`, which fails with `ProtocolStatus::InvalidEnum` by default.
- Lengths are written with a u32 prefix by `ByteWriter::bytes` and `ByteWriter::text` and are
  bounded by `kMaxEncodedStringBytes`; `ByteWriter` records the first failure and never throws.
  Optionals are written as a presence byte (`0` absent, `1` present) followed by the value.
- `encode_*` returns an empty string when the payload cannot be encoded within the bound;
  `decode_*` returns a `DecodeOutcome` with a non-OK status and a `StructuredError`.

## Message types

`MessageType` (`include/cluster_fabric/protocol.hpp:65`) has one value per message plus the
reserved `Invalid = 0`. Directions below are as implemented by the coordinator's session loop
(`src/coordinator_network.cpp:552`) and by `RackAgent` (`src/rack_agent.cpp`).

| Value | Message | Direction | Purpose |
| --- | --- | --- | --- |
| 0 | `Invalid` | - | Reserved. Sent to the coordinator it is rejected as `unknown_message`. |
| 1 | `Hello` | peer -> coordinator | Opens a session: protocol version, cluster identity, boot identity, role and build string. Mandatory first message. |
| 2 | `HelloAck` | coordinator -> peer | Accepts or refuses the handshake and returns coordinator epoch, cluster epoch, topology epoch, topology generation and lifecycle. |
| 3 | `RegisterPublisher` | peer -> coordinator | Claims a rack identity under a publisher authority and a rack generation, carrying the rack lifecycle state, composition summary and origin label. |
| 4 | `RegisterAck` | coordinator -> peer | Result of the registration, with the current epochs and membership generation. |
| 5 | `PublishRequest` | peer -> coordinator | Carries a complete `MutationRequest`. The coordinator overwrites the request's boot identity with the one bound to the session. |
| 6 | `PublishResult` | coordinator -> peer | Carries the complete `MutationResult`, including outcome, reason, error stage, generations and explanation. |
| 7 | `Heartbeat` | peer -> coordinator | Liveness and authority check: cluster identity, authority and timestamp. |
| 8 | `HeartbeatAck` | coordinator -> peer | Accepts or refuses the heartbeat, returning current epochs and lifecycle. |
| 9 | `SnapshotRequest` | peer -> coordinator | Requests a compact `SnapshotView` for a cluster identity. |
| 10 | `SnapshotResponse` | coordinator -> peer | The view, or a refusal with reason and detail. The full canonical state is never sent. |
| 11 | `ValidateSnapshotRequest` | peer -> coordinator | Asks whether a previously obtained `SnapshotView` is still current. |
| 12 | `ValidateSnapshotResponse` | coordinator -> peer | `SnapshotValidation` with typed stale reasons and subjects. |
| 13 | `RevalidateRequest` | peer -> coordinator | Requests revalidation of recovered state under current authority, listing the racks concerned. |
| 14 | `RevalidateResponse` | coordinator -> peer | Accepts or refuses, returning lifecycle and the full `ReadinessEvaluation`. |
| 15 | `Shutdown` | peer -> coordinator | Asks the coordinator to stop; carries a reason string. |
| 16 | `Error` | either direction | A `StructuredError` describing a protocol-level failure. The coordinator sends it for framing, handshake and unknown-message failures and then closes the session. |

The coordinator accepts only `Hello`, `RegisterPublisher`, `PublishRequest`,
`Heartbeat`, `SnapshotRequest`, `ValidateSnapshotRequest`, `RevalidateRequest` and
`Shutdown`. A response type sent to it (`HelloAck`, `RegisterAck`, `PublishResult`,
`HeartbeatAck`, `SnapshotResponse`, `ValidateSnapshotResponse`,
`RevalidateResponse`, `Error`) or `Invalid` hits the default branch and is answered with an
`Error` frame carrying reason code `unknown_message`, after which the session ends
(`src/coordinator_network.cpp:723`).

## The mandatory HELLO-first handshake

1. The peer connects and sends `Hello` with its protocol version, cluster identity and boot
   identity.
2. The coordinator validates the payload, then the protocol version, then the cluster identity
   (`src/coordinator_network.cpp:506`). Any failure produces a `HelloAck` with
   `accepted = false`, a `RejectionReason` (`Malformed` for a bad payload or version,
   `WrongCluster` for a foreign cluster) and a detail string, and the session ends immediately.
3. On success the coordinator records the peer's boot identity on the session
   (`session->boot`) and sets `handshake_done`. Every later mutation submitted on that session
   has its authority boot identity replaced by the session's, so a peer cannot claim another
   incarnation's identity.
4. Any non-`Hello` message before the handshake completes is answered with an `Error` frame
   carrying reason code `handshake_required` ("HELLO must be the first message on a session")
   and the session ends (`src/coordinator_network.cpp:543`).
5. `RackAgent::connect()` performs the same sequence in the client direction: connect, HELLO,
   expect `HelloAck`, then `RegisterPublisher` and expect `RegisterAck`. It refuses a
   handshake whose `HelloAck` carries a different cluster or an unsupported version
   (`src/rack_agent.cpp:664`).

## How a session is fenced

Fencing is what makes process death safe.

- A session records the boot identity it was opened with. When the session ends for any reason,
  `detach_session` checks whether that boot identity currently owns live evidence - a rack record
  or a link whose boot identity matches (`src/coordinator_network.cpp:363`). A read-only client
  that only sent `Hello` and a snapshot query owns nothing and never grows the fenced-authority
  set.
- If it does own evidence, the coordinator queues a fence request for the commit thread with reason
  `session_closed` when `CoordinatorConfig::fence_on_disconnect` is true (the default) and
  `session_closed_evidence_stale` otherwise (`src/coordinator_network.cpp:392`).
- The commit thread applies the fence (`apply_fence`, `src/coordinator_engine.cpp:1858`):
  every rack record owned by that boot loses `authoritative_current`, its currentness becomes
  `RevalidationRequired`, its evidence is marked revalidation-required and it gets a
  non-authoritative reason; every link owned by that boot is marked the same way; a
  `FencedAuthority` record is appended; the cluster generation and the health generation are
  advanced; and the result is persisted. The fenced set is sorted by boot identity and is never
  cleared, so the incarnation can never publish again.
- Two facts worth stating plainly: `fence_on_disconnect` is only read to choose the fence
  *reason* string. The header documents an alternative behaviour ("when false the rack is marked
  DEGRADED ..."), but the implementation fences in both cases. And because fencing is a commit, a
  coordinator that is stopping drains and rejects pending requests rather than persisting fences
  (`src/coordinator_network.cpp:798`).

## Replay and staleness rejection

A request is rejected before it can change anything if any of the following holds. The
`RejectionReason` is the canonical machine-readable spelling from
`to_string(RejectionReason)` (`src/mutation.cpp:65`).

| Condition | RejectionReason | Stage |
| --- | --- | --- |
| Request kind is `Unknown`, cluster identity invalid, or a required field is absent | `REJECT_MALFORMED` | `Decode` |
| Request names a different cluster | `REJECT_WRONG_CLUSTER` | `ValidateAuthority` |
| Cluster is `RETIRED` or `RETIRING` | `REJECT_RETIRED` | `ValidateAuthority` |
| Session boot identity is in the fenced set | `REJECT_STALE_RACK_BOOT` | `ValidateAuthority` |
| Boot identity does not match the authoritative incarnation of the rack | `REJECT_NOT_AUTHORIZED` | `ValidateAuthority` |
| Publisher is not the durable authority for the rack | `REJECT_NOT_AUTHORIZED` | `ValidateAuthority` |
| Publisher identity is bound to another rack | `REJECT_CONFLICT` | `ValidateAuthority` |
| `authority.cluster_epoch` is not current | `REJECT_STALE_CLUSTER_EPOCH` | `ValidateGeneration` |
| `authority.coordinator_epoch` is not current | `REJECT_STALE_COORDINATOR_EPOCH` | `ValidateGeneration` |
| `authority.publication` is not newer than the last accepted publication | `REJECT_STALE_PUBLICATION` | `ValidateGeneration` |
| Rack generation is not the authoritative one | `REJECT_STALE_RACK_GENERATION` | `ValidateGeneration` |
| `authority.topology_epoch` is not current | `REJECT_STALE_TOPOLOGY_EPOCH` | `ValidateGeneration` |
| Rack or domain referenced by the request does not exist | `REJECT_UNKNOWN_RACK`, `REJECT_UNKNOWN_DOMAIN` | `ValidateReference` |
| Domain or relationship is structurally invalid (bad member, bad endpoint, bad direction) | `REJECT_INVALID_DOMAIN`, `REJECT_INVALID_RELATIONSHIP` | `ValidateReference` |
| Subject is not ready for the requested change | `REJECT_NOT_READY` | `ValidateAuthority` |
| A bound would be exceeded, or the pending queue is full | `REJECT_LIMIT_EXCEEDED` | `ValidateReference` or `Commit` |
| The candidate state violates an invariant | `REJECT_INVARIANT_VIOLATION` | `VerifyInvariants` |
| Persistence failed | `REJECT_PERSISTENCE_FAILED` | `Persist` |
| The coordinator is stopping | `REJECT_SHUTTING_DOWN` | `Commit` |
| Internal failure | `REJECT_INTERNAL` | depends |

Rules that make replay ineffective:

- **Publication counters are monotonic per publisher.** A publication generation that is not
  newer than the accepted one is rejected as `REJECT_STALE_PUBLICATION`, so a captured frame
  cannot be replayed to move state backwards.
- **Boot identity fencing is permanent.** Once an incarnation is fenced, every request carrying
  that boot identity is rejected as `REJECT_STALE_RACK_BOOT`. Restarting a process produces a new
  boot identity, which is what lets the coordinator tell a restart apart from a replay.
- **Coordinator epoch advances on restart.** Every request and heartbeat must carry the current
  coordinator epoch; a peer that reconnects after a restart with a stale epoch is refused with
  `REJECT_STALE_COORDINATOR_EPOCH` and must re-handshake.
- **Cluster epoch guards the cluster identity itself.** A request authored under a superseded
  cluster epoch is refused with `REJECT_STALE_CLUSTER_EPOCH`.
- **Identical republication is idempotent, not a replay.** A byte-identical rack publication under
  the same generation is answered with `MutationOutcome::NoChange` and reason
  `idempotent` rather than being applied twice (`src/coordinator_engine.cpp:666`).
- **Bound views are validated, never trusted.** A snapshot view is compared field by field
  against current authority and returns typed `SnapshotStaleReason` values: `WRONG_CLUSTER`,
  `CLUSTER_EPOCH_ADVANCED`, `COORDINATOR_EPOCH_ADVANCED`,
  `CLUSTER_GENERATION_ADVANCED`, `MEMBERSHIP_CHANGED`, `TOPOLOGY_EPOCH_SUPERSEDED`,
  `TOPOLOGY_GENERATION_ADVANCED`, `RACK_GENERATION_SUPERSEDED`, `RACK_WITHDRAWN`,
  `RACK_RETIRED`, `RACK_BOOT_FENCED`, `RACK_REVALIDATION_REQUIRED`, the per-class
  `*_DOMAIN_SUPERSEDED` reasons, `CONNECTIVITY_SUPERSEDED`, `HEALTH_SUPERSEDED`,
  `LIFECYCLE_NOT_CONSUMABLE` (`src/snapshot.cpp:17`). A view is current only when that list
  is empty, and consumable only when it is also current and the lifecycle is consumable.
- **Heartbeats are checked too.** A heartbeat with a foreign cluster, stale coordinator epoch or
  stale cluster epoch is answered with the corresponding rejection rather than accepted
  (`src/coordinator_network.cpp:629`).

## Protocol status codes

`ProtocolStatus` (`include/cluster_fabric/protocol.hpp:40`) is the transport-level outcome:
`Ok`, `BadMagic`, `UnsupportedVersion`, `TruncatedHeader`, `TruncatedPayload`,
`OversizedFrame`, `ChecksumMismatch`, `UnknownMessage`, `MalformedPayload`,
`AbsurdLength`, `TrailingGarbage`, `InvalidEnum`, `InvalidIdentity`, `ConnectionClosed`,
`IoError`, `NotConnected`, `AlreadyRunning`, `NotRunning`, `ShutdownInProgress`,
`Rejected`. The reason code in a `StructuredError` built from a status is the lower-case
form of the status name (`reason_code()`, `src/protocol.cpp:196`); hand-written reason codes
in the session loop are `oversized_frame`, `handshake_required`, `unknown_message` and
`malformed_payload`.

## Transport behaviour

- The coordinator listens on an IPv4 TCP socket bound to `CoordinatorConfig::bind_address` and
  `CoordinatorConfig::port`; port 0 selects an ephemeral port, which is reported by
  `bound_port()` (`src/coordinator_network.cpp:83`).
- Reads use `select` with a 100 ms poll interval so a session can observe the stop flag. The
  comment in the source is explicit that this is not an operation timeout, and no test may depend
  on it.
- One detached worker thread per session; sessions beyond `max_sessions` are shut down
  immediately after accept (`src/coordinator_network.cpp:417`).
- `shutdown()` is used to wake a blocked peer; sockets are closed on every exit path.

Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
