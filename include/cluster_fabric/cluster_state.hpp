// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Canonical cluster state and derived indexes.
//
// ClusterState is the single authoritative record of the cluster composition.
// Every other structure in this header is derived from it and may be rebuilt
// at any time. Queries never mutate canonical state; indexes never become
// authoritative.

#ifndef CLUSTER_FABRIC_CLUSTER_STATE_HPP
#define CLUSTER_FABRIC_CLUSTER_STATE_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "cluster_fabric/domains.hpp"
#include "cluster_fabric/evidence.hpp"
#include "cluster_fabric/generation.hpp"
#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/lifecycle.hpp"
#include "cluster_fabric/rack_reference.hpp"
#include "cluster_fabric/taxonomy.hpp"
#include "cluster_fabric/topology.hpp"

namespace cluster_fabric {

/// A process-incarnation identity that has been permanently fenced. A fenced
/// boot identity can never mutate canonical state again, even if the process
/// id is reused.
struct FencedAuthority {
  RackAgentBootId boot;
  RackId rack;
  /// Stable reason code, for example "session_closed" or "coordinator_restart".
  std::string reason;
  Timestamp fenced_at = Timestamp::unknown();

  friend bool operator==(const FencedAuthority&, const FencedAuthority&) = default;
};

/// A rack identity that has been retired. Retired identities are remembered so
/// that a stale publication can never resurrect them.
struct RetiredIdentity {
  RackId rack;
  RackGeneration last_generation;
  Timestamp retired_at = Timestamp::unknown();
  std::string reason;

  friend bool operator==(const RetiredIdentity&, const RetiredIdentity&) = default;
};

/// Cluster-side record for one rack. It embeds the narrow rack reference and
/// adds the membership authority that Cluster Fabric owns.
struct RackRecord {
  RackReference reference;
  RackMembershipState membership = RackMembershipState::Unknown;
  /// Membership generation at which the cluster-side membership last changed.
  MembershipGeneration membership_generation;
  /// Cluster generation at which this record was last accepted.
  ClusterGeneration accepted_at_generation;
  /// Evidence stamp for the membership decision itself.
  EvidenceStamp membership_evidence;
  /// True when the referenced rack generation is the authoritative one and
  /// the publishing authority is alive.
  bool authoritative_current = false;
  /// Reason code explaining why the rack is not authoritative_current.
  std::string non_authoritative_reason;

  friend bool operator==(const RackRecord&, const RackRecord&) = default;
};

/// The authoritative cluster composition.
struct ClusterState {
  ClusterId id;
  ClusterEpoch epoch;
  CoordinatorEpoch coordinator_epoch;
  ClusterGeneration generation;
  MembershipGeneration membership_generation;
  TopologyEpoch topology_epoch;
  TopologyGeneration topology_generation;
  ConnectivityGeneration connectivity_generation;
  HealthGeneration health_generation;
  ConstraintGeneration constraint_generation;
  DomainGenerations domain_generations;
  SnapshotGeneration snapshot_generation;
  PublicationGeneration publication_generation;
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;
  ReadinessContract readiness_contract;
  TopologyEpochRecord topology_record;

  /// Canonical records. std::map gives deterministic iteration order.
  std::map<RackId, RackRecord> racks;
  std::map<InterRackLinkId, InterRackLink> links;
  std::map<PlacementDomainId, PlacementDomain> placement_domains;
  std::map<CapacityDomainId, CapacityDomain> capacity_domains;
  std::map<FailureDomainId, FailureDomain> failure_domains;
  std::map<NetworkDomainId, NetworkDomain> network_domains;
  std::map<StorageDomainId, StorageDomain> storage_domains;
  std::map<PowerDomainId, PowerDomain> power_domains;
  std::map<CoolingDomainId, CoolingDomain> cooling_domains;
  std::map<LinkDomainId, LinkDomain> link_domains;
  std::map<ConstraintId, ClusterConstraint> constraints;

  /// Permanently fenced process incarnations, sorted by boot identity.
  std::vector<FencedAuthority> fenced_authorities;
  /// Retired rack identities, sorted by rack identity.
  std::vector<RetiredIdentity> retired_racks;
  /// Racks explicitly withdrawn by authority and awaiting re-admission.
  std::vector<RackId> withdrawn_racks;

  Timestamp last_mutation_at = Timestamp::unknown();
  Timestamp declared_at = Timestamp::unknown();

  [[nodiscard]] bool has_rack(const RackId& rack) const noexcept {
    return racks.find(rack) != racks.end();
  }

  /// True when the rack has a decided cluster-side membership. A rack that is
  /// only registered carries membership UNKNOWN: it exists as a record but it
  /// is not a member and must never appear in topology.
  [[nodiscard]] bool is_rack_member(const RackId& rack) const noexcept {
    const auto it = racks.find(rack);
    return it != racks.end() && it->second.membership != RackMembershipState::Unknown;
  }

  [[nodiscard]] bool is_boot_fenced(const RackAgentBootId& boot) const noexcept;
  [[nodiscard]] bool is_rack_retired(const RackId& rack) const noexcept;

  /// Number of ACTIVE racks whose referenced generation is authoritative.
  [[nodiscard]] std::size_t current_rack_count() const noexcept;

  friend bool operator==(const ClusterState&, const ClusterState&) = default;
};

/// Derived indexes. Built from ClusterState and never authoritative. Every
/// index has a corresponding verification step in invariant.hpp.
class ClusterIndexes {
 public:
  ClusterIndexes() = default;

  /// Rebuilds every index from p state. Deterministic.
  void rebuild(const ClusterState& state);

  [[nodiscard]] const std::vector<RackId>& racks_in_placement_domain(
      const PlacementDomainId& id) const noexcept;
  [[nodiscard]] const std::vector<RackId>& racks_in_capacity_domain(
      const CapacityDomainId& id) const noexcept;
  [[nodiscard]] const std::vector<RackId>& racks_in_failure_domain(
      const FailureDomainId& id) const noexcept;
  [[nodiscard]] const std::vector<RackId>& racks_in_network_domain(
      const NetworkDomainId& id) const noexcept;
  [[nodiscard]] const std::vector<PlacementDomainId>& placement_domains_of(
      const RackId& rack) const noexcept;
  [[nodiscard]] const std::vector<CapacityDomainId>& capacity_domains_of(
      const RackId& rack) const noexcept;
  [[nodiscard]] const std::vector<FailureDomainId>& failure_domains_of(
      const RackId& rack) const noexcept;
  [[nodiscard]] const std::vector<NetworkDomainId>& network_domains_of(
      const RackId& rack) const noexcept;
  [[nodiscard]] const std::vector<InterRackLinkId>& links_from(const RackId& rack) const noexcept;
  [[nodiscard]] const std::vector<InterRackLinkId>& links_to(const RackId& rack) const noexcept;
  [[nodiscard]] const std::vector<PlacementDomainId>& placement_domains_of_class(
      PlacementDomainClass klass) const noexcept;
  [[nodiscard]] const std::vector<FailureDomainId>& failure_domains_of_class(
      FailureDomainClass klass) const noexcept;
  [[nodiscard]] const std::vector<RackId>& racks_in_membership_state(
      RackMembershipState state) const noexcept;

  /// Boot identity to rack mapping, used to fence a dead process quickly.
  [[nodiscard]] const std::vector<RackId>& racks_for_boot(const RackAgentBootId& boot) const;

  [[nodiscard]] bool empty() const noexcept { return rack_count_ == 0; }

 private:
  std::size_t rack_count_ = 0;
  std::map<PlacementDomainId, std::vector<RackId>> racks_in_placement_;
  std::map<CapacityDomainId, std::vector<RackId>> racks_in_capacity_;
  std::map<FailureDomainId, std::vector<RackId>> racks_in_failure_;
  std::map<NetworkDomainId, std::vector<RackId>> racks_in_network_;
  std::map<RackId, std::vector<PlacementDomainId>> placement_of_rack_;
  std::map<RackId, std::vector<CapacityDomainId>> capacity_of_rack_;
  std::map<RackId, std::vector<FailureDomainId>> failure_of_rack_;
  std::map<RackId, std::vector<NetworkDomainId>> network_of_rack_;
  std::map<RackId, std::vector<InterRackLinkId>> links_from_;
  std::map<RackId, std::vector<InterRackLinkId>> links_to_;
  std::map<PlacementDomainClass, std::vector<PlacementDomainId>> placement_by_class_;
  std::map<FailureDomainClass, std::vector<FailureDomainId>> failure_by_class_;
  std::map<RackMembershipState, std::vector<RackId>> racks_by_membership_;
  std::map<RackAgentBootId, std::vector<RackId>> racks_by_boot_;
};

/// Result of a capacity aggregation. Aggregation is refused when member
/// quantities do not share unit and semantics, rather than summing numbers
/// that mean different things.
enum class CapacityAggregateStatus : std::uint8_t {
  Unknown = 0,
  Known = 1,
  MixedSemantics = 2,
  IncompleteEvidence = 3,
  RevalidationRequired = 4,
};

[[nodiscard]] std::string_view to_string(CapacityAggregateStatus value) noexcept;

struct CapacityAggregate {
  CapacityAggregateStatus status = CapacityAggregateStatus::Unknown;
  std::string unit;
  std::optional<double> value;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  std::string reason;
};

/// Aggregates one capacity quantity across a capacity domain. Derived results
/// are classified DERIVED and never presented as measured.
[[nodiscard]] CapacityAggregate aggregate_capacity(const ClusterState& state,
                                                   const CapacityDomainId& domain,
                                                   std::string_view unit);

/// Answers whether two racks are independent for a failure-domain class.
[[nodiscard]] DomainIndependence failure_domain_independence(const ClusterState& state,
                                                            const ClusterIndexes& indexes,
                                                            const RackId& lhs, const RackId& rhs,
                                                            FailureDomainClass klass);

/// Current reachability between two racks as recorded by explicit links.
/// UNKNOWN when no current link evidence exists in either direction.
[[nodiscard]] Reachability reachability_between(const ClusterState& state, const RackId& source,
                                                const RackId& destination);

/// Evaluates the readiness contract against canonical state. Deterministic.
[[nodiscard]] ReadinessEvaluation evaluate_readiness(const ClusterState& state);

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_CLUSTER_STATE_HPP
