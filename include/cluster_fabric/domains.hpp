// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Placement, capacity, failure, network, storage, power and cooling domains.
//
// Domains are descriptive infrastructure groups owned by Cluster Fabric. They
// never select workloads, grant capacity, or imply replication policy. Every
// domain carries its own generation, provenance and authority so a snapshot
// can be checked against the domain generation it was built from.

#ifndef CLUSTER_FABRIC_DOMAINS_HPP
#define CLUSTER_FABRIC_DOMAINS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cluster_fabric/evidence.hpp"
#include "cluster_fabric/generation.hpp"
#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/taxonomy.hpp"

namespace cluster_fabric {

/// Common header shared by every domain record.
struct DomainHeader {
  /// Provenance and freshness of the domain declaration as a whole.
  EvidenceStamp evidence;
  /// Generation of this domain record. Advanced by every accepted change.
  DomainGeneration generation;
  /// Authority that declared or last changed the domain.
  std::optional<RackPublisherId> publisher;
  ClusterEpoch cluster_epoch;
  CoordinatorEpoch coordinator_epoch;
  /// Bounded free-form label. Empty when unknown.
  std::string label;

  friend bool operator==(const DomainHeader&, const DomainHeader&) = default;
};

/// Placement domain: racks that share a descriptive placement property.
struct PlacementDomain {
  PlacementDomainId id;
  PlacementDomainClass klass = PlacementDomainClass::Unknown;
  /// Sorted, deduplicated member racks.
  std::vector<RackId> racks;
  DomainHeader header;

  friend bool operator==(const PlacementDomain&, const PlacementDomain&) = default;
};

/// A supplied aggregate quantity. The unit and provenance are preserved so
/// numbers with different semantics are never summed together.
struct CapacityQuantity {
  /// Bounded unit text, for example "bytes", "devices", "slots".
  std::string unit;
  /// Absent when the quantity is unknown. Never coerced to zero.
  std::optional<double> value;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  /// True when this value is an aggregate over member racks rather than a
  /// value supplied directly by an authority.
  bool aggregated = false;

  friend bool operator==(const CapacityQuantity&, const CapacityQuantity&) = default;
};

/// Capacity domain: a structural aggregation scope. It is not a grant and it
/// is not a workload-fit engine.
struct CapacityDomain {
  CapacityDomainId id;
  CapacityDomainClass klass = CapacityDomainClass::Unknown;
  std::vector<RackId> racks;
  /// Sorted by (unit, provenance). Supplied or derived evidence only.
  std::vector<CapacityQuantity> quantities;
  DomainHeader header;

  friend bool operator==(const CapacityDomain&, const CapacityDomain&) = default;
};

/// Failure domain: infrastructure that fails together. Overlapping failure
/// domains are legitimate and do not have to form a tree.
struct FailureDomain {
  FailureDomainId id;
  FailureDomainClass klass = FailureDomainClass::Unknown;
  std::vector<RackId> racks;
  DomainHeader header;

  friend bool operator==(const FailureDomain&, const FailureDomain&) = default;
};

/// Network domain: a connectivity scope used to classify inter-rack links.
struct NetworkDomain {
  NetworkDomainId id;
  ConnectivityClass connectivity = ConnectivityClass::Unknown;
  std::vector<RackId> racks;
  DomainHeader header;

  friend bool operator==(const NetworkDomain&, const NetworkDomain&) = default;
};

/// Storage domain: a storage locality scope.
struct StorageDomain {
  StorageDomainId id;
  std::vector<RackId> racks;
  DomainHeader header;

  friend bool operator==(const StorageDomain&, const StorageDomain&) = default;
};

/// Power domain above the racks. Parent forms an explicit hierarchy.
struct PowerDomain {
  PowerDomainId id;
  std::vector<RackId> racks;
  std::optional<PowerDomainId> parent;
  DomainHeader header;

  friend bool operator==(const PowerDomain&, const PowerDomain&) = default;
};

/// Cooling domain above the racks.
struct CoolingDomain {
  CoolingDomainId id;
  std::vector<RackId> racks;
  std::optional<CoolingDomainId> parent;
  DomainHeader header;

  friend bool operator==(const CoolingDomain&, const CoolingDomain&) = default;
};

/// Link domain: a group of inter-rack links that share fate (for example one
/// fabric plane). Used to answer failure-independence questions about links.
struct LinkDomain {
  LinkDomainId id;
  std::vector<RackId> racks;
  DomainHeader header;

  friend bool operator==(const LinkDomain&, const LinkDomain&) = default;
};

/// Per-class aggregate domain generations bound into a snapshot.
struct DomainGenerations {
  DomainGeneration placement;
  DomainGeneration capacity;
  DomainGeneration failure;
  DomainGeneration network;
  DomainGeneration storage;
  DomainGeneration power;
  DomainGeneration cooling;
  DomainGeneration link;

  [[nodiscard]] bool all_known() const noexcept {
    return placement.known() && capacity.known() && failure.known() && network.known() &&
           storage.known() && power.known() && cooling.known() && link.known();
  }

  friend bool operator==(const DomainGenerations&, const DomainGenerations&) = default;
};

/// A cluster-level constraint record. Descriptive evidence only: Cluster
/// Fabric publishes what the infrastructure was declared to satisfy and never
/// turns it into a placement decision.
struct ClusterConstraint {
  ConstraintId id;
  ConstraintKind kind = ConstraintKind::Unknown;
  /// Racks the constraint is scoped to. Sorted, deduplicated.
  std::vector<RackId> racks;
  /// Optional domain the constraint refers to, rendered as "class/id".
  std::string domain_ref;
  /// Bounded free-form statement text.
  std::string statement;
  DomainHeader header;

  friend bool operator==(const ClusterConstraint&, const ClusterConstraint&) = default;
};

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_DOMAINS_HPP
