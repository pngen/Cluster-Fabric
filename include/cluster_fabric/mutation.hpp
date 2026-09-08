// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Explicit cluster mutations and typed results.
//
// Every externally significant change to the cluster composition is expressed
// as a MutationRequest carrying the authority it was authored under. A request
// is decoded, its authority and generations are validated, referenced
// entities are resolved, a candidate state is constructed, invariants are
// verified, and only then is the candidate committed. No partial graph ever
// becomes authoritative.
//
// Failures are never reduced to a boolean: each rejection carries a typed
// reason, the affected identity, the expected and supplied generations, and a
// structured explanation.

#ifndef CLUSTER_FABRIC_MUTATION_HPP
#define CLUSTER_FABRIC_MUTATION_HPP

#include <optional>
#include <string>
#include <vector>

#include "cluster_fabric/cluster_state.hpp"
#include "cluster_fabric/domains.hpp"
#include "cluster_fabric/error.hpp"
#include "cluster_fabric/evidence.hpp"
#include "cluster_fabric/explanation.hpp"
#include "cluster_fabric/generation.hpp"
#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/lifecycle.hpp"
#include "cluster_fabric/rack_reference.hpp"
#include "cluster_fabric/topology.hpp"

namespace cluster_fabric {

enum class MutationKind : std::uint8_t {
  Unknown = 0,
  DeclareCluster = 1,
  RegisterRackPublisher = 2,
  AddRack = 3,
  UpdateRackGeneration = 4,
  MarkRackUnavailable = 5,
  WithdrawRack = 6,
  RetireRack = 7,
  RecoverRack = 8,
  PublishInterRackLink = 9,
  WithdrawInterRackLink = 10,
  PublishPlacementDomain = 11,
  PublishCapacityDomain = 12,
  PublishFailureDomain = 13,
  PublishNetworkDomain = 14,
  PublishStorageDomain = 15,
  PublishPowerDomain = 16,
  PublishCoolingDomain = 17,
  PublishLinkDomain = 18,
  PublishConstraint = 19,
  PublishHealthCurrentness = 20,
  WithdrawEvidence = 21,
  SupersedeTopology = 22,
  PublishSnapshot = 23,
  RevalidateRecoveredState = 24,
  RetireCluster = 25,
};

[[nodiscard]] std::string_view to_string(MutationKind value) noexcept;
[[nodiscard]] std::optional<MutationKind> mutation_kind_from_string(std::string_view text) noexcept;

/// The authority a request was authored under. A request that does not match
/// current authority is rejected before it can mutate anything.
struct MutationAuthority {
  ClusterEpoch cluster_epoch;
  CoordinatorEpoch coordinator_epoch;
  TopologyEpoch topology_epoch;
  TopologyGeneration topology_generation;
  /// Process incarnation that authored the request, when rack-side.
  std::optional<RackAgentBootId> boot;
  /// Durable authority entitled to publish the subject.
  std::optional<RackPublisherId> publisher;
  /// Rack the request is scoped to, when rack-scoped.
  std::optional<RackId> rack;
  /// Rack generation the author believed authoritative.
  std::optional<RackGeneration> rack_generation;
  /// Monotonic per-publisher publication counter.
  PublicationGeneration publication;

  friend bool operator==(const MutationAuthority&, const MutationAuthority&) = default;
};

/// A single mutation request. Fields are grouped by the kinds that use them;
/// the kind selects which payload is read. Unused payloads are ignored, which
/// keeps decoding and testing simple and allocation free at the API boundary.
struct MutationRequest {
  MutationKind kind = MutationKind::Unknown;
  ClusterId cluster;
  MutationAuthority authority;
  EvidenceStamp evidence;
  /// Optional operator or process note. Bounded, never authoritative.
  std::string reason;
  Timestamp requested_at = Timestamp::unknown();

  /// DECLARE_CLUSTER.
  ClusterLifecycle declared_lifecycle = ClusterLifecycle::Declared;
  ReadinessContract readiness_contract;

  /// Rack-scoped mutations.
  RackReference rack_reference;
  RackMembershipState membership = RackMembershipState::Unknown;
  /// Compare-and-swap expectation for generation changes.
  std::optional<RackGeneration> expected_rack_generation;
  std::optional<RackGeneration> new_rack_generation;

  /// Domain-scoped mutations.
  std::optional<PlacementDomain> placement_domain;
  std::optional<CapacityDomain> capacity_domain;
  std::optional<FailureDomain> failure_domain;
  std::optional<NetworkDomain> network_domain;
  std::optional<StorageDomain> storage_domain;
  std::optional<PowerDomain> power_domain;
  std::optional<CoolingDomain> cooling_domain;
  std::optional<LinkDomain> link_domain;
  std::optional<ClusterConstraint> constraint;

  /// Topology-scoped mutations.
  std::optional<InterRackLink> link;
  std::optional<InterRackLinkId> link_id;
  std::optional<TopologyEpoch> target_topology_epoch;
  std::string topology_reason;

  /// Health and reachability publications.
  HealthState health = HealthState::Unknown;
  Reachability reachability = Reachability::Unknown;

  /// Which evidence class a withdrawal applies to, for example "rack_health".
  std::string evidence_selector;

  /// REVALIDATE_RECOVERED_STATE: the racks whose evidence must be current.
  /// An empty list means every member rack.
  std::vector<RackId> racks;

  /// Bounded textual rendering used by diagnostics and the CLI.
  [[nodiscard]] std::string describe() const;
};

enum class MutationOutcome : std::uint8_t {
  /// The mutation changed canonical state.
  Accepted = 0,
  /// The mutation was valid and idempotent: state was already as requested.
  NoChange = 1,
  /// The mutation was refused. Nothing changed.
  Rejected = 2,
  /// The mutation was refused and the subject now requires revalidation.
  RevalidationRequired = 3,
};

[[nodiscard]] std::string_view to_string(MutationOutcome value) noexcept;

enum class RejectionReason : std::uint8_t {
  None = 0,
  StaleClusterEpoch,
  StaleCoordinatorEpoch,
  StaleRackBoot,
  StaleRackGeneration,
  StaleTopologyEpoch,
  StalePublication,
  NotAuthorized,
  Conflict,
  UnknownRack,
  UnknownDomain,
  InvalidDomain,
  InvalidRelationship,
  Retired,
  WrongCluster,
  LimitExceeded,
  Malformed,
  NotReady,
  ShuttingDown,
  PersistenceFailed,
  InvariantViolation,
  Internal,
};

[[nodiscard]] std::string_view to_string(RejectionReason value) noexcept;
[[nodiscard]] std::optional<RejectionReason> rejection_reason_from_string(std::string_view text) noexcept;

/// Typed result of one mutation.
struct MutationResult {
  MutationOutcome outcome = MutationOutcome::Rejected;
  RejectionReason reason = RejectionReason::None;
  StructuredError error;
  Explanation explanation;

  ClusterEpoch cluster_epoch;
  CoordinatorEpoch coordinator_epoch;
  ClusterGeneration cluster_generation;
  MembershipGeneration membership_generation;
  TopologyEpoch topology_epoch;
  TopologyGeneration topology_generation;
  ConnectivityGeneration connectivity_generation;
  HealthGeneration health_generation;
  ConstraintGeneration constraint_generation;
  SnapshotGeneration snapshot_generation;
  PublicationGeneration publication_generation;
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;

  [[nodiscard]] bool accepted() const noexcept {
    return outcome == MutationOutcome::Accepted || outcome == MutationOutcome::NoChange;
  }
  [[nodiscard]] bool changed() const noexcept { return outcome == MutationOutcome::Accepted; }

  [[nodiscard]] static MutationResult rejected(RejectionReason reason, ErrorStage stage,
                                               std::string subject, std::string detail,
                                               std::string reason_code = std::string());
};

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_MUTATION_HPP
