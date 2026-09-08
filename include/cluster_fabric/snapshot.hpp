// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Immutable cluster snapshots and currentness validation.
//
// A snapshot binds the cluster generation, cluster epoch, coordinator epoch,
// membership generation, topology epoch and generation, every per-class domain
// generation, the connectivity and health generations, and the rack-generation
// map it was built from. Consumers validate a snapshot against current
// authority before acting on it, and receive typed stale reasons when it is no
// longer current.

#ifndef CLUSTER_FABRIC_SNAPSHOT_HPP
#define CLUSTER_FABRIC_SNAPSHOT_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cluster_fabric/cluster_state.hpp"
#include "cluster_fabric/explanation.hpp"
#include "cluster_fabric/generation.hpp"
#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/lifecycle.hpp"

namespace cluster_fabric {

/// One rack-generation binding inside a snapshot.
struct RackGenerationBinding {
  RackId rack;
  RackGeneration generation;
  RackMembershipState membership = RackMembershipState::Unknown;
  RackCurrentness currentness = RackCurrentness::Unknown;
  bool authoritative_current = false;
  RackAgentBootId boot;

  friend bool operator==(const RackGenerationBinding&, const RackGenerationBinding&) = default;
};

/// Why a snapshot is no longer current. Every reason is explicit; a snapshot
/// is never silently reinterpreted.
enum class SnapshotStaleReason : std::uint8_t {
  None = 0,
  ClusterEpochAdvanced,
  CoordinatorEpochAdvanced,
  ClusterGenerationAdvanced,
  MembershipChanged,
  TopologyEpochSuperseded,
  TopologyGenerationAdvanced,
  RackGenerationSuperseded,
  RackWithdrawn,
  RackRetired,
  RackBootFenced,
  RackRevalidationRequired,
  PlacementDomainSuperseded,
  CapacityDomainSuperseded,
  FailureDomainSuperseded,
  NetworkDomainSuperseded,
  StorageDomainSuperseded,
  PowerDomainSuperseded,
  CoolingDomainSuperseded,
  LinkDomainSuperseded,
  ConnectivitySuperseded,
  HealthSuperseded,
  LifecycleNotConsumable,
  WrongCluster,
};

[[nodiscard]] std::string_view to_string(SnapshotStaleReason value) noexcept;

/// Result of validating a snapshot against current authority.
struct SnapshotValidation {
  /// True when every bound generation still matches current state.
  bool current = false;
  /// True when the snapshot may be consumed as a current composition.
  bool consumable = false;
  /// Typed reasons, sorted and deduplicated.
  std::vector<SnapshotStaleReason> reasons;
  /// Subjects that caused each reason, in the same order.
  std::vector<std::string> subjects;
  Explanation explanation;

  [[nodiscard]] bool ok() const noexcept { return current && consumable; }
  [[nodiscard]] bool requires_revalidation() const noexcept;
  [[nodiscard]] std::string describe() const;
};

/// An immutable view of the authoritative composition at one generation.
///
/// The snapshot holds a shared, immutable copy of the canonical state. It is
/// logically immutable: no accessor returns a mutable reference and the
/// underlying state is never modified after construction.
class ClusterSnapshot {
 public:
  ClusterSnapshot() = default;

  /// Builds a snapshot from p state. The snapshot takes an immutable copy.
  [[nodiscard]] static ClusterSnapshot capture(const ClusterState& state);

  [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
  [[nodiscard]] const ClusterId& cluster() const noexcept { return state_->id; }
  [[nodiscard]] ClusterEpoch cluster_epoch() const noexcept { return state_->epoch; }
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept {
    return state_->coordinator_epoch;
  }
  [[nodiscard]] ClusterGeneration cluster_generation() const noexcept { return state_->generation; }
  [[nodiscard]] MembershipGeneration membership_generation() const noexcept {
    return state_->membership_generation;
  }
  [[nodiscard]] TopologyEpoch topology_epoch() const noexcept { return state_->topology_epoch; }
  [[nodiscard]] TopologyGeneration topology_generation() const noexcept {
    return state_->topology_generation;
  }
  [[nodiscard]] ConnectivityGeneration connectivity_generation() const noexcept {
    return state_->connectivity_generation;
  }
  [[nodiscard]] HealthGeneration health_generation() const noexcept {
    return state_->health_generation;
  }
  [[nodiscard]] ConstraintGeneration constraint_generation() const noexcept {
    return state_->constraint_generation;
  }
  [[nodiscard]] const DomainGenerations& domain_generations() const noexcept {
    return state_->domain_generations;
  }
  [[nodiscard]] SnapshotGeneration snapshot_generation() const noexcept {
    return state_->snapshot_generation;
  }
  [[nodiscard]] PublicationGeneration publication_generation() const noexcept {
    return state_->publication_generation;
  }
  [[nodiscard]] ClusterLifecycle lifecycle() const noexcept { return state_->lifecycle; }

  /// Frozen canonical state. Never mutable.
  [[nodiscard]] const ClusterState& state() const noexcept { return *state_; }

  /// Rack-generation map bound into the snapshot, sorted by rack identity.
  [[nodiscard]] const std::vector<RackGenerationBinding>& rack_bindings() const noexcept {
    return bindings_;
  }

  [[nodiscard]] std::optional<RackGeneration> rack_generation(const RackId& rack) const noexcept;

  /// Deterministic digest over the semantic content of the snapshot. Stable
  /// across processes, runs and machines for identical state.
  [[nodiscard]] const std::string& semantic_digest() const noexcept { return digest_; }

  /// Validates this snapshot against current authority.
  [[nodiscard]] SnapshotValidation validate(const ClusterState& current) const;

  /// Convenience: validates against another snapshot's state.
  [[nodiscard]] SnapshotValidation validate(const ClusterSnapshot& current) const;

  friend bool operator==(const ClusterSnapshot&, const ClusterSnapshot&) = default;

 private:
  void rebuild_bindings();
  std::shared_ptr<const ClusterState> state_;
  std::vector<RackGenerationBinding> bindings_;
  std::string digest_;
};

/// Computes the semantic digest of canonical state. Public so that the CLI and
/// benchmarks can verify determinism directly.
[[nodiscard]] std::string semantic_digest_of(const ClusterState& state);

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_SNAPSHOT_HPP
