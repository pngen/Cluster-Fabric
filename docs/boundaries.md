# Cluster Fabric boundaries

Cluster Fabric owns exactly one layer: the composition of a cluster out of rack-level
authorities. Every other cluster-level concern belongs to another system, and Cluster Fabric
keeps only the narrow contract it needs to compose the cluster and to say whether that
composition is current.

The rule the code follows is: **record what the infrastructure is, never what a higher-level
runtime should do about it.**

## What Cluster Fabric owns

| Owned concern | Types and sources |
| --- | --- |
| Rack membership | `RackMembershipState`, `RackRecord`, `MutationKind::AddRack`, `MarkRackUnavailable`, `WithdrawRack`, `RetireRack`, `RecoverRack` (`include/cluster_fabric/cluster_state.hpp`, `include/cluster_fabric/mutation.hpp`) |
| Inter-rack connectivity | `InterRackLink`, `LinkDirection`, `ConnectivityClass`, `Reachability`, `BandwidthClass`, `LatencyClass`, `InterRackLinkId` (`include/cluster_fabric/topology.hpp`) |
| Cluster topology | `TopologyEpoch`, `TopologyGeneration`, `TopologyEpochRecord`, `TopologyCurrentness`, `MutationKind::SupersedeTopology` (`include/cluster_fabric/topology.hpp`) |
| Placement, capacity, failure, network, storage, power, cooling and link domains | `PlacementDomain`, `CapacityDomain`, `FailureDomain`, `NetworkDomain`, `StorageDomain`, `PowerDomain`, `CoolingDomain`, `LinkDomain`, `DomainHeader`, `DomainGenerations` (`include/cluster_fabric/domains.hpp`) |
| Failure-domain independence answers | `DomainIndependence`, `failure_domain_independence()` (`include/cluster_fabric/topology.hpp`, `include/cluster_fabric/cluster_state.hpp`) |
| Locality classes | `PlacementDomainClass`, `FailureDomainClass`, `CapacityDomainClass`, `ConnectivityClass`, `BandwidthClass`, `LatencyClass`, `AcceleratorVendor` (`include/cluster_fabric/taxonomy.hpp`) |
| Topology epochs and generations | `TopologyEpoch`, `TopologyGeneration`, `ConnectivityGeneration`, `HealthGeneration`, `DomainGeneration` and `DomainGenerations` (`include/cluster_fabric/generation.hpp`) |
| Rack generations (referenced, not owned) | `RackGeneration`, `RackCurrentness`, `RackReference::generation` (`include/cluster_fabric/generation.hpp`, `include/cluster_fabric/rack_reference.hpp`) |
| Reachability | `Reachability`, `reachability_between()`, `MutationKind::PublishHealthCurrentness` (`include/cluster_fabric/taxonomy.hpp`, `include/cluster_fabric/cluster_state.hpp`) |
| Hierarchy above the racks | `PowerDomain::parent`, `CoolingDomain::parent`, domain membership lists (`include/cluster_fabric/domains.hpp`) |
| Cluster lifecycle | `ClusterLifecycle`, `ReadinessContract`, `ReadinessEvaluation`, `ReadinessBlocker`, `MutationKind::DeclareCluster`, `RetireCluster`, `RevalidateRecoveredState` (`include/cluster_fabric/lifecycle.hpp`, `include/cluster_fabric/mutation.hpp`) |
| Cluster-level constraints (as evidence) | `ClusterConstraint`, `ConstraintKind`, `MutationKind::PublishConstraint` (`include/cluster_fabric/domains.hpp`, `include/cluster_fabric/taxonomy.hpp`) |
| Currentness of everything above | `EvidenceStamp`, `EvidenceProvenance`, `Freshness`, `Durability`, `ClusterSnapshot`, `SnapshotStaleReason` (`include/cluster_fabric/evidence.hpp`, `include/cluster_fabric/snapshot.hpp`) |

Exactly one cluster is in scope: `ClusterId`, `ClusterState` and the readiness contract are
all per-cluster, and a request for a different cluster is rejected with
`RejectionReason::WrongCluster` (`src/coordinator_engine.cpp:472`).

## What Cluster Fabric must not absorb

| System | Cluster Fabric keeps this narrow contract instead |
| --- | --- |
| Rack Fabric | `RackReference` (`include/cluster_fabric/rack_reference.hpp`): rack identity, `RackGeneration`, `RackLifecycleState`, `RackCurrentness`, a bounded `RackCompositionSummary`, bounded endpoints, bounded failure-domain hints, the publisher authority, the boot identity and the evidence stamp. No device inventory, no rack-internal topology, no rack-internal scheduling. `RackLifecycleState` is documented as "reported by the rack authority (Rack Fabric). Cluster Fabric relays it; it does not own it." |
| Fabric Scheduler | Nothing. `MutationKind` contains no scheduling, placement, admission or queue operation, and `ClusterConstraint` is explicitly "descriptive evidence only: Cluster Fabric publishes what the infrastructure was declared to satisfy and never turns it into a placement decision." |
| Communication Planner | `InterRackLink` records a declared or observed relationship with its classification and evidence. The header states Cluster Fabric "is not a route planner: it records what relationship was declared and observed, with provenance, and it never invents a bandwidth or a latency that was not supplied." `nominal_bandwidth_bps` and `nominal_latency_nanos` exist only as optionals that are present when explicitly supplied. |
| Collective Scheduler | Nothing. There is no collective, group, communicator or rank type anywhere in `include/cluster_fabric`. |
| Collective Fabric | Nothing. Inter-rack relationships are pairwise (`source`, `destination`) with an explicit `LinkDirection`; a unidirectional relationship does not imply the reverse. |
| Resource Broker | `CapacityDomain` is a structural aggregation scope. The header states it "is not a grant and it is not a workload-fit engine." `aggregate_capacity()` returns a `CapacityAggregate` with `CapacityAggregateStatus` and refuses to sum numbers with different semantics (`MIXED_SEMANTICS`, reason code `member_quantities_have_different_semantics`). |
| Capacity Fabric | `CapacityQuantity` preserves unit, provenance and an `aggregated` flag; a value is `std::optional<double>` and "absent when the quantity is unknown. Never coerced to zero." Cluster Fabric never derives a usable-capacity claim. |
| Reservation Fabric | Nothing. There is no reservation, lease, hold, allocation or quota type. `ConstraintKind::CapacityFloor` is a published requirement record, not an enforced floor. |
| Congestion Fabric | Nothing. `HealthState` and `Reachability` are per-rack and per-link states; there is no traffic, queue-depth, congestion or back-pressure model, and no rate control. |
| NVLink / NVSwitch / GPU-Direct / RDMA-buffer / DPU / CXL / PCIe / storage fabrics | `RackEndpoint` (`RackEndpointId`, optional `NetworkDomainId`, optional `LinkDomainId`, `ConnectivityClass`, evidence) and `AcceleratorClassSummary` (`AcceleratorVendor`, bounded family label, optional device count, optional device memory). The header states "Cluster Fabric does not inventory the device behind the endpoint", and `AcceleratorVendor` records the vendor only ("it never dispatches to it"). `StorageDomain` is a locality scope; no device, path, buffer or link-level technology is modelled. |
| Heterogeneous Accelerator Federation | `AcceleratorClassSummary` and `AcceleratorVendor` only. Vendor, family, device count and device memory are recorded as optional evidence; there is no capability model, no kernel selection and no dispatch. |
| Cross-Cluster State Fabric | Nothing. `ClusterState` holds one `ClusterId`; there is no federation, peering, multi-cluster view, remote-cluster reference or cluster-of-clusters type. A request naming a different cluster is rejected, and a snapshot bound to a different cluster validates as `SnapshotStaleReason::WrongCluster`. |

## The types that enforce the boundary

- **`RackReference`** is the boundary object itself. Its header states: "Rack Fabric owns
  everything inside a rack. Cluster Fabric owns the composition above it. The boundary is this
  type." It carries no device inventory and no rack-internal structure.
- **`RackCompositionSummary`** makes the boundary numeric: every count
  (`cpu_sockets`, `cpu_cores`, `host_memory_bytes`, `nic_count`, `switch_count`,
  `device_count`, `device_memory_bytes`) is `std::optional`, so "not reported" is
  representable and can never be mistaken for "zero".
- **Domain records** (`PlacementDomain`, `CapacityDomain`, `FailureDomain`,
  `NetworkDomain`, `StorageDomain`, `PowerDomain`, `CoolingDomain`, `LinkDomain`) are
  descriptive groups with a generation, a publisher, a cluster epoch, a coordinator epoch and an
  evidence stamp. They "never select workloads, grant capacity, or imply replication policy"
  (`include/cluster_fabric/domains.hpp`).
- **`ConstraintKind`** enumerates only `PlacementScope`, `ConnectivityRequirement`,
  `FailureDomainIndependence`, `CapacityFloor` and `MaintenanceWindow`. These are evidence
  kinds, not enforcement kinds: the records "describe requirements the infrastructure was declared
  to satisfy. Cluster Fabric publishes them; it does not enforce placement."
- **`Reachability` and `ConnectivityClass`** keep physical and logical connectivity apart.
  `ConnectivityClass::RoutedPath`, `Overlay` and `ManagementOnly` are distinct from
  `DirectFabric`, and the topology header states "process connectivity to a coordinator is not
  proof of a data path". `reachability_between()` returns `Unknown` unless a link carries current
  evidence in the right direction.
- **`EvidenceProvenance`** keeps claims attributable. `REPORTED` is "asserted by another runtime
  or by an operator; Cluster Fabric relays the claim without having observed it"; `SYNTHETIC` is
  never a physical fact; `RECONSTRUCTED` "is a memory of a past declaration, not a current
  observation, and never satisfies a currentness check".
- **`CapacityAggregateStatus`** refuses to fabricate a number: `Unknown`,
  `MixedSemantics`, `IncompleteEvidence` and `RevalidationRequired` are all valid answers,
  and a derived aggregate is classified `DERIVED` and "never presented as measured".
- **`DomainIndependence::Unknown`** is the default answer to "are these two racks independent for
  this failure-domain class?", and the code returns it whenever current domain evidence does not
  cover both racks (`src/cluster_state.cpp:462`).
- **The CUDA probe** (`include/cluster_fabric/cuda_probe.hpp`) exists only to prove that a
  composition can bind real lower-level evidence. Its header states "Cluster Fabric is not a GPU
  runtime and does not schedule the device", and when CUDA is absent it "reports UNSUPPORTED rather
  than inventing a result".

## What the boundary implies operationally

- Cluster Fabric never asks another system for work; it is queried, and it answers with evidence
  and typed reasons.
- A consumer that wants a placement decision has to make it. Cluster Fabric gives it current
  racks, current links, current domains, currentness of each of those, and the exact reason any
  bound view went stale.
- Adding a concern to Cluster Fabric means adding a type to the list above. There is no
  extension point, plugin interface, dynamic loading or callback that lets an outside system push
  behaviour into the coordinator; the only observer is `CommitHook`, which is documented as
  test-only and must not call back into the coordinator.

Copyright 2026 Summon Software Labs. Licensed under the Apache License, Version 2.0.
