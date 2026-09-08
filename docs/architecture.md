# Cluster Fabric architecture

Cluster Fabric is a vendor-neutral, cluster-level infrastructure runtime. It composes one
cluster out of rack-level authorities and answers one question, continuously and with typed
evidence: **what is the authoritative composition of this cluster at this instant, which of
that composition is still backed by current evidence, and exactly why is any older view no
longer current?**

Everything else in the system - placing work, scheduling collectives, granting capacity,
routing traffic - is deliberately outside the boundary (see [boundaries.md](boundaries.md)).

## The exact question

For one cluster identity, Cluster Fabric records and answers:

- which racks are members, under which `RackGeneration`, with which cluster-side membership state;
- which inter-rack relationships were declared or observed, with direction, connectivity class,
  reachability, optional nominal bandwidth and latency, and the failure domains they belong to;
- which placement, capacity, failure, network, storage, power, cooling and link domains cover
  those racks, and at which domain generation;
- which cluster-level constraints were declared;
- which epochs and generations bind all of the above (`ClusterEpoch`, `CoordinatorEpoch`,
  `TopologyEpoch`, `ClusterGeneration`, `MembershipGeneration`, `TopologyGeneration`,
  `ConnectivityGeneration`, `HealthGeneration`, `ConstraintGeneration`,
  `SnapshotGeneration`, `PublicationGeneration`, and the per-class `DomainGenerations`);
- which of those facts are backed by *current* evidence, where current means a provenance that
  is eligible (`MEASURED` or `REPORTED`), a known timestamp and `FRESH` freshness
  (`EvidenceStamp::is_current` in `include/cluster_fabric/evidence.hpp`);
- for any consumer holding an earlier view, the typed `SnapshotStaleReason` that makes that view
  stale.

The answer is delivered as immutable `ClusterSnapshot` objects, typed `MutationResult`
objects and structured `Explanation` objects. No answer is ever a bare boolean.

## Single-writer, multi-reader coordinator

`ClusterCoordinator` (`include/cluster_fabric/coordinator.hpp`) is the single authority for one
cluster. The model is stated in the header and implemented in `src/detail/coordinator_impl.hpp`:

- **One writer.** Exactly one commit thread runs `Impl::run_commit()`. It is the only code that
  mutates canonical `ClusterState`. It is started by `start()` and joined by `stop()`.
- **Serialized submission.** Every caller of `submit()` pushes a `QueueItem` onto a bounded
  queue (bound `kMaxPendingRequests`, 4096) and blocks on its own `RequestSlot` condition
  variable until the commit thread has produced a `MutationResult`. A full queue is rejected with
  `RejectionReason::LimitExceeded` and reason code `queue_full`; a submission while shutting
  down is rejected with `RejectionReason::ShuttingDown` and reason code `shutting_down`.
- **Many readers.** `snapshot()`, `state_copy()`, `readiness()`, `invariants()`,
  `validate()` and the `explain_*` family read canonical state under `state_mutex` and return
  copies or immutable snapshots. Readers never block the commit path for longer than the copy.
- **Derived indexes are never authoritative.** `ClusterIndexes` is rebuilt lazily inside
  `Impl::with_state()` whenever `indexes_version != state_version`, and is verified against
  canonical records by `check_indexes()`.

## Transaction pipeline

`ErrorStage` (`include/cluster_fabric/error.hpp`) names the stages of the transactional
sequence. The implementation order in `src/coordinator_engine.cpp` (`apply_and_commit`) is:

```text
  submit()                       enqueue, block on slot
    |
    v
  run_commit()                   the single commit thread
    |
    +-- decode                    kind known, cluster identity valid        -> ErrorStage::Decode
    +-- validate authority        cluster/coordinator epoch, boot fencing,
    |                             publisher ownership, retired cluster      -> ValidateAuthority
    +-- validate generations      cluster epoch, coordinator epoch,
    |                             rack generation, publication counter,
    |                             topology epoch                           -> ValidateGeneration
    +-- validate references       racks, domains, links, constraints,
    |                             endpoints, bounds                        -> ValidateReference
    +-- apply under undo journal  StateJournal records every touched map
    |                             entry, rack record and scalar field
    +-- recompute lifecycle       evaluate_readiness(canonical)
    +-- verify invariants         check_invariants(canonical, nullptr)      -> VerifyInvariants
    +-- advance cluster generation bump(canonical.generation)               -> Commit
    +-- persist                   persist_now() -> PersistenceStore::save() -> Persist
    +-- publish                   touch_state(): ++state_version,
                                  indexes_valid = false                    -> Publish
    +-- fill_generations()        copy every generation into the result
```

A failure at any stage calls `journal.rollback()`, which replays the recorded undo entries in
reverse order (`StateJournal::rollback`), so canonical state is restored exactly. A candidate
that fails an invariant is rejected with `RejectionReason::InvariantViolation`, stage
`VerifyInvariants` and reason code `invariant_violation`; a failed persistence commit is
rejected with `RejectionReason::PersistenceFailed`, stage `Persist` and the persistence status
as its reason code. Neither case publishes anything.

Two observations that are verifiable in the sources and are recorded here rather than smoothed
over:

- The file header comment of `src/coordinator_engine.cpp` lists "persist -> advance generations
  -> publish"; the code advances the cluster generation *before* persisting (generation bump at
  `src/coordinator_engine.cpp:1820`, `persist_now` at `src/coordinator_engine.cpp:1835`).
  The rollback covers both orders, so the outcome is the same, but the comment and the code do not
  agree on the sequence.
- `ErrorStage::ConstructCandidate`, `ErrorStage::Publish` and `ErrorStage::Shutdown` are
  declared and rendered by `to_string(ErrorStage)` but are never used as a failure stage by the
  implementation; the stages actually reported are `Decode`, `ValidateAuthority`,
  `ValidateGeneration`, `ValidateReference`, `VerifyInvariants`, `Commit`, `Persist`,
  `Encode`, `Transport` and `Recover`.

### Outcomes

`MutationOutcome` distinguishes `Accepted` (state changed), `NoChange` (valid and idempotent:
state was already as requested), `Rejected` (refused, nothing changed) and
`RevalidationRequired` (refused, and the subject must republish). `MutationResult::accepted()`
is true for `Accepted` and `NoChange`; `changed()` is true only for `Accepted`.

## Copy-on-write snapshots

`ClusterSnapshot::capture(const ClusterState&)` (`src/snapshot.cpp`) copies canonical state into
a `std::shared_ptr<const ClusterState>`, rebuilds the rack-generation bindings and computes the
semantic digest. Copying a `ClusterSnapshot` copies the shared pointer, not the state.

- The snapshot is logically immutable: every accessor returns `const&`, `state()` returns
  `const ClusterState&`, and the stored state is `const`. The coordinator never mutates a state
  object a snapshot holds; it mutates `canonical` and publishes a new `state_version`.
- `bindings_` binds every member rack to the `RackGeneration`, membership state, currentness,
  `authoritative_current` flag and `RackAgentBootId` that were current at capture time.
- `validate(const ClusterState&)` and `validate(const ClusterSnapshot&)` compare the bound
  generations against current authority and return typed `SnapshotStaleReason` values with the
  subject that caused each one. `consumable` additionally requires
  `is_consumable_lifecycle(current.lifecycle)`.
- `validate(const SnapshotView&)` (the wire form, implemented in
  `src/coordinator_network.cpp`) performs the same comparison for a compact view received from a
  peer, including rack bindings, and additionally reports `MembershipChanged` for racks added
  after the view was built.

## Deterministic semantic digest

`semantic_digest_of(const ClusterState&)` (`src/snapshot.cpp`) renders canonical state into a
FNV-1a 64-bit hash and returns it as 16 lowercase hexadecimal digits.

- Mixing: `hash ^= byte; hash *= 1099511628211` per input byte, with a separator step
  (`hash ^= 0x7c; hash *= 1099511628211`) after every field.
- The initial value used by `DigestBuilder` is `1469598103934665603`. The canonical FNV-1a
  64-bit offset basis is `14695981039346656037`; the constant in the source is one digit shorter.
  The digest is therefore deterministic and stable for this implementation, but it is not the
  canonical FNV-1a 64 initial value, so a third-party FNV-1a implementation will not reproduce it.
- Input order is fixed: cluster identity, every epoch and generation, lifecycle, topology record
  reason and timestamp, then racks in `RackId` order, links in `InterRackLinkId` order, then each
  domain class in identity order, then constraints, retired identities, withdrawn racks and fenced
  authorities. Optional numerics are encoded as value plus a presence flag so an absent value never
  collides with a present zero.
- Because `ClusterState` stores every record in a `std::map`, iteration order is deterministic;
  the digest is stable across processes, runs and machines for identical state.

## The UNKNOWN-first doctrine

Every type in Cluster Fabric defaults to an explicit UNKNOWN and no code path promotes UNKNOWN to
a positive value:

- `EvidenceProvenance::Unknown` is the default of every `EvidenceStamp`;
  `Freshness::Unknown` is the default freshness; `Timestamp{}` (zero) means "unknown" and
  `Timestamp::known()` is false for it.
- `is_current_evidence_provenance()` accepts only `MEASURED` and `REPORTED`. `SYNTHETIC`,
  `ESTIMATED`, `RECONSTRUCTED` and `UNKNOWN` can never satisfy a current-evidence check.
- Strong counters reserve zero for "unknown / never published" (`StrongCounter::known()`), and
  `StrongCounter::next()` returns `std::nullopt` on overflow instead of wrapping.
- Every numeric field of `RackCompositionSummary` and `AcceleratorClassSummary` is
  `std::optional`; an absent count is UNKNOWN and is never coerced to zero. `CapacityQuantity`
  uses `std::optional<double>`.
- `Reachability::Unknown`, `DomainIndependence::Unknown`, `ConnectivityClass::Unknown`,
  `RackCurrentness::Unknown`, `HealthState::Unknown`, `RackMembershipState::Unknown`,
  `RackLifecycleState::Unknown` and `CapacityAggregateStatus::Unknown` are all the default and
  are never silently converted into a positive classification.
- A default-constructed `StrongId` is the explicit UNKNOWN identity: empty, rejected by every
  parser and never equal to a parsed identity.
- The invariant `UNKNOWN_NEVER_POSITIVE` enforces that an authoritative-current rack never carries
  UNKNOWN provenance (`src/invariant.cpp:211`), and `RECOVERED_EVIDENCE_NOT_CURRENT` enforces
  that recovered evidence is never marked authoritative current (`src/invariant.cpp:215`).

## Layering

```text
  applications and consumers
    apps/coordinator_main.cpp   apps/rack_agent_main.cpp   apps/inspect_main.cpp
    examples/01_embedded_cluster.cpp   examples/02_consumer_contract.cpp
    examples/03_cuda_rack_evidence.cpp   benchmarks/bench_cluster.cpp   benchmarks/bench_queries.cpp
      |
      |  public API (installed headers, namespace cluster_fabric)
      v
  include/cluster_fabric/cluster_fabric.hpp        umbrella header
      |
      +-------------------------+-------------------------+
      |                         |                         |
  coordinator.hpp           rack_agent.hpp            protocol.hpp
      |                         |                         |
      v                         v                         v
  coordinator.cpp         rack_agent.cpp            protocol.cpp
  coordinator_engine.cpp                            (framing + payload codecs)
  coordinator_network.cpp
      |                         |                         |
      +------------+------------+-------------+-----------+
                   |                          |
             snapshot.hpp                persistence.hpp
                   |                          |
                   v                          v
             snapshot.cpp                persistence.cpp
                   |                          |
                   +-----------+--------------+
                               |
                       cluster_state.hpp + invariant.hpp
                               |
                     cluster_state.cpp + invariant.cpp
                               |
        -------------------------------------------------------
        identity  generation  evidence  taxonomy  lifecycle
        rack_reference  topology  domains  mutation  error
        explanation  limits  version
        -------------------------------------------------------
        header-only or single-translation-unit primitives, no dependencies
```

The core library has no third-party dependency. On Windows it links `ws2_32`; elsewhere it links
`Threads::Threads` (`CMakeLists.txt`). CUDA is an optional, separate component
(`cluster_fabric_cuda`) that a consumer links explicitly.

## Module map

| Header | Implementation | Responsibility |
| --- | --- | --- |
| `cluster_fabric.hpp` | none (umbrella) | Includes every public header. |
| `cluster_state.hpp` | `src/cluster_state.cpp` | Canonical `ClusterState`, `ClusterIndexes`, capacity aggregation, independence and reachability queries, readiness evaluation. |
| `coordinator.hpp` | `src/coordinator.cpp`, `src/coordinator_engine.cpp`, `src/coordinator_network.cpp` | Coordinator lifecycle, public API, explanations, commit engine, durable projection, sockets and sessions. |
| `cuda_probe.hpp` | `src/cuda_probe.cpp`; `src/cuda/cuda_probe.cu` when CUDA is enabled | Optional accelerator evidence probe and its `RackReference` projection. |
| `domains.hpp` | header-only records | `DomainHeader` and every domain record, `CapacityQuantity`, `ClusterConstraint`. |
| `error.hpp` | `src/error.cpp` | `ErrorCategory`, `ErrorStage`, `StructuredError` and its deterministic rendering. |
| `evidence.hpp` | `src/evidence.cpp` | Provenance, freshness, durability, `Timestamp`, `Clock`, `EvidenceStamp`. |
| `explanation.hpp` | `src/explanation.cpp` | Structured explanations, factor sorting and bounding. |
| `generation.hpp` | `src/generation.cpp` | `StrongCounter`, every epoch and generation alias, counter text parsing and rendering. |
| `identity.hpp` | `src/identity.cpp` | `StrongId`, every identity alias, identity and label validation, boot-id generation. |
| `invariant.hpp` | `src/invariant.cpp` | `InvariantId`, `check_invariants`, `check_indexes`. |
| `lifecycle.hpp` | `src/lifecycle.cpp` | Cluster, rack-membership and rack-lifecycle states, `ReadinessContract`, `ReadinessEvaluation`. |
| `limits.hpp` | header-only constants | Every hard resource bound. |
| `mutation.hpp` | `src/mutation.cpp` | `MutationKind`, `MutationAuthority`, `MutationRequest`, `MutationResult`, `RejectionReason`. |
| `persistence.hpp` | `src/persistence.cpp` | `PersistedState`, container encoding and decoding, `FilePersistenceStore`, `MemoryPersistenceStore`. |
| `protocol.hpp` | `src/protocol.cpp` | Frame codec, `MessageType`, every message payload, `ByteWriter`/`ByteReader`, `crc32`. |
| `rack_agent.hpp` | `src/rack_agent.cpp` | Rack-side publisher process: connect, register, publish, supersede, revalidate, heartbeat. |
| `rack_reference.hpp` | `src/rack_reference.cpp` | The narrow rack contract, its validation and canonicalization. |
| `snapshot.hpp` | `src/snapshot.cpp` | `ClusterSnapshot`, `RackGenerationBinding`, `SnapshotValidation`, semantic digest. |
| `synthetic.hpp` | `src/synthetic.cpp` | Deterministic synthetic cluster laboratory. |
| `taxonomy.hpp` | `src/taxonomy.cpp` | Descriptive classification enums and their spellings. |
| `topology.hpp` | `src/topology.cpp` | `InterRackLink`, `TopologyEpochRecord`, `TopologyCurrentness`, `DomainIndependence`. |
| `version.hpp` | `src/version.cpp` | Product and format versions, protocol and persistence magic, build identity. |

`synthetic.hpp` is listed above because it is part of the public surface and is included by
`cluster_fabric.hpp`, and `src/synthetic.cpp` is listed in `CLUSTER_FABRIC_SOURCES` in
`CMakeLists.txt`; that translation unit is **not present in the working tree**, so the functions
declared in `synthetic.hpp` have no definition in this checkout. See
[testing.md](testing.md) for the same observation about the test sources.

`src/detail/coordinator_impl.hpp` is internal, is not installed, and is not part of the public
contract.

## Thread and lock discipline

The audited locking contract is documented at `src/detail/coordinator_impl.hpp:126`:

| Lock | Guards | Notes |
| --- | --- | --- |
| `state_mutex` | canonical `ClusterState`, derived `ClusterIndexes`, `state_version`, `indexes_version` | Always taken last; never taken while `queue_mutex` or `sessions_mutex` is held. |
| `queue_mutex` | the commit request queue and the `stopping` flag | Released before the commit thread touches canonical state. |
| `sessions_mutex` | the session registry `std::map<std::uint64_t, std::shared_ptr<Session>>` | Sessions are joined after the registry lock is released. |
| `Session::write_mutex` | that one session's socket writes | Per session. |

Threads:

- **accept thread** (`run_accept`): one per coordinator, started by `start()`, joined by
  `stop()`. Accepts, enforces `max_sessions`, and spawns one **detached** session worker per
  connection. Each worker removes itself from the registry in `detach_session` before returning,
  which is what makes the drain loop in `stop()` safe.
- **commit thread** (`run_commit`): one per coordinator. Holds `state_mutex` for the duration of
  one mutation's apply/verify/persist/publish sequence.
- **session workers**: one per accepted connection. They read frames, call `submit()` (which
  blocks on the commit thread) and write responses under the session's `write_mutex`.

Two facts that contradict the audited comment are recorded here rather than smoothed over:

- `state_mutex` **is** held across persistence I/O. `apply_and_commit` takes
  `std::lock_guard<std::mutex> lock(state_mutex)` at `src/coordinator_engine.cpp:1768` and
  calls `persist_now()` at line 1835 inside that scope, and `persist_now()` calls
  `PersistenceStore::save()`, which performs file I/O. The header comment states the lock is
  never held across persistence I/O.
- `CommitHook::after_publish` is declared (`include/cluster_fabric/coordinator.hpp:127`) but is
  never invoked; only `before_candidate` and `before_commit` are called
  (`src/coordinator_engine.cpp:1765` and `1832`).

Socket I/O never happens under `state_mutex`: `send_frame` is called from session workers with
only the session's `write_mutex` held, and `with_state()` bodies never write to a socket.

## Recovery on coordinator restart

`start()` calls `Impl::load_durable_state()` before any listener exists
(`src/coordinator.cpp:172`). Its behaviour:

1. If a `PersistenceStore` is configured, `load()` is called. `MissingFile` is a normal
   "fresh cluster" case and is recorded as a note; any other non-OK status is recorded as a
   rejected container with the persistence status and message in `RecoveryReport`.
2. The next `CoordinatorEpoch` is `previous + 1`, where `previous` is the recorded
   `last_coordinator_epoch` when a container loaded, and 1 otherwise. A coordinator without a
   store therefore starts at coordinator epoch 2 in every incarnation and has no memory of earlier
   incarnations.
3. If durable state loaded, `from_persisted_state` (`src/coordinator_engine.cpp:203`) projects it
   onto canonical state and applies the recovery rules:
   - every rack reference gets provenance `RECONSTRUCTED`, freshness
     `REVALIDATION_REQUIRED`, `currentness = RevalidationRequired`,
     `authoritative_current = false` and
     `non_authoritative_reason = "recovered_state_requires_revalidation"`;
   - every recovered rack's last known boot identity is added to `fenced_authorities` with reason
     `coordinator_restart` if it is not fenced already;
   - recovered links lose reachability and health (set to `Unknown`) and their evidence is marked
     revalidation-required;
   - recovered domain and constraint evidence is re-evaluated against the current clock
     (`EvidenceStamp::refresh`), which may downgrade it to `STALE`;
   - membership, retired identities, withdrawn racks and fenced authorities are restored, because
     they are declarations rather than observations;
   - `RecoveryReport` reports counts, the previous and current coordinator epoch, whether the
     epoch advanced and whether revalidation is required; notes are sorted for determinism.
4. Lifecycle is recomputed from the recovered state by `evaluate_readiness`; a cluster with
   recovered racks that have not republished lands in `REVALIDATION_REQUIRED`.
5. `indexes_valid` is cleared and `state_version` incremented, so the first reader rebuilds the
   derived indexes from the recovered canonical state.

Recovery never trusts dynamic observation. What is persisted, what is discarded and how a
container is validated are specified in [persistence.md](persistence.md).

Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
