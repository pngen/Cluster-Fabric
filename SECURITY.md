# Security policy

## Reporting a vulnerability

Report suspected vulnerabilities privately. The repository publishes one canonical location:

1. Open a private security advisory on the project page recorded in `CMakeLists.txt`
   (`HOMEPAGE_URL`, `https://github.com/pngen/Cluster-Fabric`). This is the preferred channel
   because it keeps the report, the reproduction and the fix in one place and lets a fix be
   prepared before disclosure.
2. If the report does not need to be private - for example a hardening suggestion or a missing
   bound - open a normal issue on the same project page.

There is no security email address published in the repository, so please do not rely on one.

Include, where you can:

- the exact version (`version_banner()` prints "Cluster Fabric 1.0.0 (<build>)"), the toolchain
  and the platform;
- whether the issue is in the library, the wire protocol, the persistence format, or one of the
  three executables;
- the smallest input or sequence that reproduces it, and the observed typed status, reason code or
  assertion;
- the impact you believe it has.

Expect an acknowledgement on the project page. Please do not include real infrastructure
identities, hostnames or credentials in a report; the code never needs them to reproduce a
protocol or persistence issue.

## Scope

In scope:

- the core library (`include/cluster_fabric` and `src`): identity and generation parsing,
  evidence classification, the mutation pipeline, invariants, snapshots and the semantic digest;
- the wire protocol (`src/protocol.cpp`): framing, bounds, payload decoding, the version tag and
  the CRC32 check;
- the persistence format (`src/persistence.cpp`): the container header, checksum construction,
  decoding validation and the atomic-replace path;
- the three executables in `apps/`: argument parsing, the read-only inspection path and the
  process lifecycle;
- the optional CUDA probe (`src/cuda_probe.cpp`, `src/cuda/cuda_probe.cu`) and its fallback.

Out of scope:

- the CUDA runtime or driver that the optional probe links; report those to the vendor;
- the operating system, the network, or any middlebox between a rack agent and a coordinator;
- deployments that expose the coordinator listener to an untrusted network, or that run without
  the operator-supplied endpoint being the intended one;
- denial of service caused by exhausting a bound that is documented and enforced
  (`kMaxSessions`, `kMaxPendingRequests`, `kMaxFramePayloadBytes`, `kMaxPersistenceBytes`,
  and the rest of `include/cluster_fabric/limits.hpp`);
- the synthetic laboratory's generated scenarios: they are classified `SYNTHETIC` and are not
  claims about real hardware.

## Security posture

**No telemetry.** The library and the executables contain no telemetry, analytics, usage
reporting, update check or phone-home of any kind. There is no HTTP client, no DNS lookup and no
external service reference anywhere in the sources.

**No network egress except what the operator configures.** The only sockets in the repository are:

- the coordinator's listener, bound to `CoordinatorConfig::bind_address` and
  `CoordinatorConfig::port` (IPv4 TCP; the CLI default is `127.0.0.1` with an ephemeral port);
- the outbound TCP connection that `RackAgent` makes to the coordinator endpoint the operator
  supplies on the command line or in `RackAgentConfig`;
- the outbound TCP connection that `cluster_fabric_inspect --coordinator` makes to the endpoint
  the operator supplies.

Nothing else opens a socket. There is no inbound path other than the coordinator listener.

**Bounded decoding of untrusted input.** Every length, count and enum is checked before a byte is
read or allocated:

- the frame decoder rejects a declared payload above the configured maximum and above
  `kAbsoluteMaxFramePayloadBytes` before reading it (`ProtocolStatus::OversizedFrame`), and
  rejects a declared length that exceeds the bytes present (`ProtocolStatus::TruncatedPayload`);
- payload decoders reject trailing bytes (`ProtocolStatus::TrailingGarbage`) and out-of-range
  enums (`ProtocolStatus::InvalidEnum`);
- every single encoded string is bounded by `kMaxEncodedStringBytes`;
- the container decoder rejects absurd counts, trailing garbage, bad magic, unsupported versions
  and reserved flags before it decodes any record, and enforces `kMaxPersistenceBytes` while
  reading the file;
- identities are validated against a narrow alphabet (ASCII letters, digits and
  `- _ . : ```) with a 128-byte bound, and `.`, `..` and any embedded `..` are rejected, so
  an identity can never be used for path traversal, shell interpolation or framing injection;
- `ByteWriter` and `ByteReader` record the first failure and never throw; the public API does
  not throw (no `throw` appears in any public header; the only `throw` in the tree is the test
  harness's failure signal).

**No shell-out.** There is no `system`, `popen`, `CreateProcess`, `ShellExecute`,
`_spawn` or `exec` anywhere in the sources. The executables are the only processes Cluster
Fabric starts, and they start none.

**No dynamic loading.** There is no `dlopen`, `LoadLibrary`, `GetProcAddress` or `dlsym`.
There is no plugin interface, no script engine and no configuration file that is executed. The
only extension point in the API is `CommitHook`, which is documented as a deterministic test
observer and is installed in-process by the caller.

**No third-party code.** The core library has no third-party dependency (`NOTICE`). On Windows
it links the system `ws2_32`; elsewhere it links `Threads::Threads`. The optional CUDA probe
links the CUDA runtime the user installs.

**Deterministic and auditable behaviour.** Every rejection is a typed result with a stable reason
code and a structured explanation; nothing is decided by a timeout. There is no hidden retry that
could mask a failure: a failed persistence commit rolls the mutation back and reports
`REJECT_PERSISTENCE_FAILED`.

## Explicit non-goals

Cluster Fabric does not provide, and does not attempt to provide:

- **authentication or transport security.** The protocol is plaintext IPv4 TCP with no TLS, no
  message authentication and no peer credential. A peer proves only that it knows a cluster
  identity, a publisher identity and a boot identity; the coordinator checks those against its
  authoritative records, and that is all.
- **confidentiality or integrity of data in transit.** A frame is protected only by a CRC32, which
  detects accidental corruption, not tampering. Anyone who can write to the socket can rewrite a
  frame and its checksum.
- **a hardened network service.** The listener is a plain TCP accept loop with bounded sessions.
  Rate limiting, connection filtering, SYN-flood protection and network policy are the operator's
  responsibility.
- **audit logging.** Explanations and statistics are in-process; there is no tamper-evident log,
  no remote syslog and no retention policy.
- **secret storage.** There are no credentials, keys or tokens to store, and the persistence
  container is not encrypted. A state file contains cluster composition, identities and
  timestamps, and should be protected as operational metadata.
- **authorization beyond the composition model.** A publisher may only publish the rack it is the
  durable authority for, under its own boot identity and the current epochs; there is no role
  model, no per-field permission and no operator identity.
- **sandboxing or isolation.** The library runs in the caller's process with the caller's
  privileges. It does not restrict what a consumer does with the data it returns.

Because of the first two non-goals, run the coordinator on a trusted interface or inside a
protected network path, and treat every `--bind` value other than a loopback or private address
as a deliberate decision.

Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
