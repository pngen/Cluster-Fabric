// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Invariants.
//
// Invariants are checked continuously on the commit path and are available as
// a public function so tests, the CLI and the benchmark harness can validate
// any state they hold. A violated invariant blocks the commit: a candidate
// state that violates an invariant never becomes authoritative.

#ifndef CLUSTER_FABRIC_INVARIANT_HPP
#define CLUSTER_FABRIC_INVARIANT_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cluster_fabric/cluster_state.hpp"

namespace cluster_fabric {

enum class InvariantId : std::uint8_t {
  Unknown = 0,
  SingleClusterIdentity,
  ClusterEpochKnown,
  CoordinatorEpochKnown,
  ClusterGenerationKnown,
  GenerationsMonotonic,
  MembershipGenerationConsistent,
  TopologyEpochKnown,
  TopologyGenerationConsistent,
  RackIdentitiesValid,
  RackGenerationAuthoritative,
  RackGenerationUnique,
  RackNotFenced,
  RackNotRetired,
  RackMembershipConsistent,
  RackReferenceBounded,
  LinkEndpointsResolve,
  LinkDirectionConsistent,
  LinkNoSelfLoop,
  LinkIdentitiesUnique,
  DomainMembersResolve,
  DomainClassConsistent,
  DomainGenerationsConsistent,
  ConstraintReferencesResolve,
  IndexMatchesCanonical,
  SnapshotMapMatchesState,
  LifecycleConsistent,
  UnknownNeverPositive,
  RecoveredEvidenceNotCurrent,
  FencedAuthorityConsistent,
  CountsWithinBounds,
  AccountingBaseline,
};

[[nodiscard]] std::string_view to_string(InvariantId value) noexcept;

struct InvariantViolation {
  InvariantId id = InvariantId::Unknown;
  std::string subject;
  std::string detail;

  friend bool operator==(const InvariantViolation&, const InvariantViolation&) = default;
};

struct InvariantReport {
  /// Sorted by (id, subject) so reports are deterministic.
  std::vector<InvariantViolation> violations;

  [[nodiscard]] bool ok() const noexcept { return violations.empty(); }
  [[nodiscard]] std::string describe() const;
};

/// Checks every invariant of canonical state. When p indexes is supplied the
/// derived indexes are verified against canonical records as well.
[[nodiscard]] InvariantReport check_invariants(const ClusterState& state,
                                               const ClusterIndexes* indexes = nullptr);

/// Verifies only the derived indexes.
[[nodiscard]] InvariantReport check_indexes(const ClusterState& state,
                                            const ClusterIndexes& indexes);

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_INVARIANT_HPP
