# Cluster Fabric invariants

Every invariant lives in `src/invariant.cpp` and is identified by an `InvariantId`
(`include/cluster_fabric/invariant.hpp`). `check_invariants(state, indexes)` checks canonical
state, and - when an index set is supplied - also verifies the derived indexes through
`check_indexes`. The report is sorted by (id, subject, detail) so two runs over the same state
produce identical output (`finish()` in `src/invariant.cpp:82`).

Where the invariants run:

- on every commit, before the candidate is published: `apply_and_commit` calls
  `check_invariants(canonical, nullptr)` (`src/coordinator_engine.cpp:1809`);
- on demand through the public API: `ClusterCoordinator::invariants()` calls
  `check_invariants(state, &indexes)` (`src/coordinator.cpp:272`);
- directly by any caller: `check_invariants` and `check_indexes` are public functions.

## The invariants

The "canonical spelling" column is exactly what `to_string(InvariantId)` returns.

| InvariantId | Canonical spelling | What it enforces | Why |
| --- | --- | --- | --- |
| `Unknown` | `UNKNOWN` | Nothing. It is the reserved zero value and the fallback rendering. | A violation with an unknown id would be unactionable, so the enumerator is never emitted. |
| `SingleClusterIdentity` | `SINGLE_CLUSTER_IDENTITY` | `state.id` is not empty. | Canonical state must name the cluster it is authoritative for; an unnamed state could be applied to the wrong cluster. |
| `ClusterEpochKnown` | `CLUSTER_EPOCH_KNOWN` | `state.epoch` is non-zero. | Zero means "never declared". A cluster with an unknown epoch has no authority to reject stale requests against. |
| `CoordinatorEpochKnown` | `COORDINATOR_EPOCH_KNOWN` | `state.coordinator_epoch` is non-zero. | Fencing depends on the coordinator incarnation being identifiable. |
| `ClusterGenerationKnown` | `CLUSTER_GENERATION_KNOWN` | `state.generation` is non-zero. | The cluster generation is the monotonic stamp every consumer validates against. |
| `GenerationsMonotonic` | `GENERATIONS_MONOTONIC` | `state.membership_generation` is non-zero. | This id is emitted for a zero membership generation; a declared cluster must start its membership counter at 1. |
| `MembershipGenerationConsistent` | `MEMBERSHIP_GENERATION_CONSISTENT` | `membership_generation <= generation`; each rack record's `membership_generation <= state.membership_generation`; `withdrawn_racks` is sorted and unique; every withdrawn entry exists with membership `Withdrawn`; every rack whose membership is `Withdrawn` appears in `withdrawn_racks`. | Membership changes must be visible in the cluster generation, per-rack counters cannot run ahead of the cluster, and the withdrawn list must be a faithful, deterministic mirror of membership state. |
| `TopologyEpochKnown` | `TOPOLOGY_EPOCH_KNOWN` | `state.topology_epoch` is non-zero. | Links and snapshots bind to a topology epoch; zero cannot be bound. |
| `TopologyGenerationConsistent` | `TOPOLOGY_GENERATION_CONSISTENT` | `topology_generation` is non-zero; `topology_record.epoch` equals `topology_epoch`; `topology_record.generation` equals `topology_generation`; `topology_generation <= generation`; every link's `topology_epoch` equals the cluster topology epoch; every link's `topology_generation` does not exceed the cluster topology generation. | The epoch record, the cluster counters and every link must agree, otherwise a consumer could validate a link against the wrong topology regime. |
| `RackIdentitiesValid` | `RACK_IDENTITIES_VALID` | Each `racks` map key equals `record.reference.rack`. | The map key is the authoritative identity; a record that disagrees could be looked up under one name and reported under another. |
| `RackGenerationAuthoritative` | `RACK_GENERATION_AUTHORITATIVE` | Every rack reference has a non-zero `RackGeneration`; a rack with `authoritative_current` is never at generation zero. | Generation zero means "unknown"; an authoritative rack must be bound to a real generation. |
| `RackGenerationUnique` | `RACK_GENERATION_UNIQUE` | **No check emits this id.** The enumerator is declared and rendered, but `check_invariants` never reports it. | Documented here because it is part of the enum; it currently provides no enforcement. |
| `RackNotFenced` | `RACK_NOT_FENCED` | A rack with `authoritative_current` never has a fenced boot identity; a fenced authority's rack is never authoritative current under that same boot. | Fencing is permanent: a killed or superseded process incarnation must never be able to own live evidence again. |
| `RackNotRetired` | `RACK_NOT_RETIRED` | A retired rack identity is never authoritative current; the retired list is strictly sorted by rack identity. | Retirement is permanent and remembered, and the list must be deterministic. |
| `RackMembershipConsistent` | `RACK_MEMBERSHIP_CONSISTENT` | No rack record has membership `Unknown`; per-rack membership generation does not exceed the cluster membership generation; the withdrawn list is sorted and unique; withdrawn entries and membership state agree in both directions. | Membership must always be a decided value, and the two representations of withdrawal must never diverge. |
| `RackReferenceBounded` | `RACK_REFERENCE_BOUNDED` | `endpoints.size() <= kMaxRackEndpoints` (256) and `failure_domain_hints.size() <= kMaxFailureDomainRefs` (256). | Bounds are part of the public contract; a record that exceeds them was not accepted by validation and must not exist in canonical state. |
| `LinkEndpointsResolve` | `LINK_ENDPOINTS_RESOLVE` | Both link endpoints are member racks; every failure domain referenced by a link exists. | A link to a non-member rack or an unknown domain would make the graph inconsistent and every downstream answer unreliable. |
| `LinkDirectionConsistent` | `LINK_DIRECTION_CONSISTENT` | No link has `LinkDirection::Unknown`. | Direction is semantic: a unidirectional relationship does not imply the reverse, so an unknown direction cannot be interpreted. |
| `LinkNoSelfLoop` | `LINK_NO_SELF_LOOP` | `source != destination`. | A self-link carries no inter-rack meaning and would corrupt connectivity answers. |
| `LinkIdentitiesUnique` | `LINK_IDENTITIES_UNIQUE` | Each `links` map key equals `link.id`. | Same reason as rack identities: the key is authoritative. |
| `DomainMembersResolve` | `DOMAIN_MEMBERS_RESOLVE` | Every domain member rack is a member of the cluster; every domain member list is sorted and unique. | Domains are the basis of placement, failure-independence and locality answers; a dangling member or an unsorted list makes those answers non-deterministic or false. |
| `DomainClassConsistent` | `DOMAIN_CLASS_CONSISTENT` | Placement, capacity and failure domains have a non-`Unknown` `klass`; network domains have a non-`Unknown` `connectivity`. Storage, power, cooling and link domains pass a constant so they cannot violate this id. | A domain with an unknown class cannot be used to answer a class-scoped question, and would silently match nothing. |
| `DomainGenerationsConsistent` | `DOMAIN_GENERATIONS_CONSISTENT` | For each of the eight domain classes, no individual record generation exceeds the aggregate `DomainGenerations` value for that class. | The aggregate is what snapshots bind; if a record ran ahead of it, a consumer could hold a snapshot that claims to cover a generation it does not. |
| `ConstraintReferencesResolve` | `CONSTRAINT_REFERENCES_RESOLVE` | Every rack referenced by a constraint is a member of the cluster. | Constraints are published as evidence about this cluster; a dangling reference would make the evidence meaningless. |
| `IndexMatchesCanonical` | `INDEX_MATCHES_CANONICAL` | `check_indexes` verifies: member lists of placement, capacity, failure and network domains; the placement and failure domain lists of every rack; `links_from` and `links_to` for every link; the membership index; and the boot index. | Indexes are a performance structure that must never become authoritative; this is the proof that they are exactly a projection of canonical records. |
| `SnapshotMapMatchesState` | `SNAPSHOT_MAP_MATCHES_STATE` | `snapshot_generation <= publication_generation`. | A snapshot can never claim a publication that has not happened. |
| `LifecycleConsistent` | `LIFECYCLE_CONSISTENT` | A state whose lifecycle is `READY` must have a satisfied readiness evaluation; a `RETIRED` cluster has no rack that is authoritative current. | The lifecycle label is a claim about the same state; READY must not be assertable when the contract is unsatisfied, and RETIRED must not leave live authority behind. |
| `UnknownNeverPositive` | `UNKNOWN_NEVER_POSITIVE` | A rack with `authoritative_current` never carries `EvidenceProvenance::Unknown`. | The core doctrine, enforced: an authoritative fact must have a provenance, and absence of evidence must never be presented as positive evidence. |
| `RecoveredEvidenceNotCurrent` | `RECOVERED_EVIDENCE_NOT_CURRENT` | A rack with `authoritative_current` never carries `EvidenceProvenance::Reconstructed`. | Recovered state is a memory of a past declaration, not a current observation; marking it authoritative current would launder stale facts as live ones. |
| `FencedAuthorityConsistent` | `FENCED_AUTHORITY_CONSISTENT` | The fenced-authority list is strictly sorted by boot identity. | Deterministic iteration order for reports and digests, and a cheap way to detect duplicate fencing entries. |
| `CountsWithinBounds` | `COUNTS_WITHIN_BOUNDS` | `racks.size() <= kMaxRacksPerCluster` (16384), `links.size() <= kMaxInterRackLinks` (262144), `constraints.size() <= kMaxConstraints` (4096), and no domain class exceeds `kMaxDomainsPerClass` (16384). | Every bound in `limits.hpp` is a hard contract; canonical state must never hold more than the bounds that decoding and mutation enforce. |
| `AccountingBaseline` | `ACCOUNTING_BASELINE` | **No check emits this id.** The enumerator is declared and rendered, but `check_invariants` never reports it. | Documented here because it is part of the enum; it currently provides no enforcement. |

## Hard validity before ranking

The invariant check is a gate, not a score.

- If `check_invariants` reports any violation, the commit path rolls the candidate back
  (`src/coordinator_engine.cpp:1810`) and returns `RejectionReason::InvariantViolation` with
  stage `VerifyInvariants` and reason code `invariant_violation`. Canonical state, the published
  snapshot and every derived index keep the previous value. There is no partial acceptance, no
  "warn and continue", and no way for a caller to opt out.
- No classification can outrank validity. Readiness, lifecycle, independence, reachability and
  locality are answers *about* a state; if the state violates a structural rule, those answers are
  computed over a state that is discarded. `LifecycleConsistent` closes the loop in the other
  direction: a state may not label itself `READY` while its own readiness contract is unsatisfied.
- Ranking is expressed only through typed, inspectable results - `ReadinessEvaluation`,
  `DomainIndependence`, `Reachability`, `CapacityAggregateStatus`, `SnapshotValidation` -
  and each of those has an explicit "not enough evidence" value. A consumer that needs an ordering
  applies it to those results; Cluster Fabric never reorders, guesses or fills gaps to make an
  answer look complete.

## Why UNKNOWN never silently becomes positive

UNKNOWN is a first-class value with a defined meaning, and the code has four independent
mechanisms that keep it from turning into a positive claim:

1. **Defaults are UNKNOWN.** `EvidenceProvenance::Unknown`, `Freshness::Unknown`,
   `Timestamp{}`, `HealthState::Unknown`, `Reachability::Unknown`,
   `RackMembershipState::Unknown`, `RackLifecycleState::Unknown`,
   `RackCurrentness::Unknown`, `DomainIndependence::Unknown`,
   `CapacityAggregateStatus::Unknown`, and a default-constructed `StrongId` are all the
   UNKNOWN value.
2. **Optionality is explicit.** Counts, memory sizes, nominal bandwidth and latency, hop counts,
   parent domains and capacity values are `std::optional`. There is no "absent means zero"
   convention anywhere, and the digest encodes presence separately from value so an absent zero can
   never collide with a present zero.
3. **Eligibility rules reject weak evidence.** `is_current_evidence_provenance()` accepts only
   `MEASURED` and `REPORTED`; `EvidenceStamp::is_current()` additionally requires a known
   timestamp and `FRESH` freshness; `EvidenceStamp::requires_revalidation()` is true for
   `RevalidationRequired` freshness and for `RECONSTRUCTED` provenance. A stale, synthetic,
   estimated, reconstructed or unknown observation therefore fails every currentness check instead
   of passing a weaker one.
4. **Invariants make it structural.** `UNKNOWN_NEVER_POSITIVE` forbids an authoritative-current
   rack with UNKNOWN provenance, and `RECOVERED_EVIDENCE_NOT_CURRENT` forbids an
   authoritative-current rack with recovered provenance. Because these run before publication, a
   code path that tried to promote UNKNOWN would be rejected by the commit path itself rather than
   silently accepted.

The same doctrine appears in the answers: `failure_domain_independence()` returns
`DomainIndependence::Unknown` when no current domain of the requested class covers both racks
(`src/cluster_state.cpp:462`), and `reachability_between()` returns
`Reachability::Unknown` when no link carries current evidence in the right direction
(`src/cluster_state.cpp:516`). The explanations say so in words:
"no current link carries current evidence for this rack pair; UNKNOWN is not evidence of
connectivity" (`src/coordinator.cpp:479`) and "no current domain of this class covers both racks,
so independence cannot be asserted" (`src/coordinator.cpp:446`).

Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
