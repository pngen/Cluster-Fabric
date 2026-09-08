// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Cluster, rack-membership and readiness lifecycles.
//
// READY is earned. It is never granted because one rack exists. The readiness
// contract is explicit, configurable and evaluated deterministically from
// canonical state; every blocker is reported with a stable code.

#ifndef CLUSTER_FABRIC_LIFECYCLE_HPP
#define CLUSTER_FABRIC_LIFECYCLE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/taxonomy.hpp"

namespace cluster_fabric {

/// Lifecycle of the cluster authority itself.
///
///   DECLARED              - identity declared; no rack has been accepted yet.
///   FORMING               - racks are being accepted; contract not satisfied.
///   PARTIAL               - the configured rack set is knowingly incomplete,
///                           but the present part is coherent and current.
///   READY                 - the readiness contract is satisfied.
///   DEGRADED              - the contract was satisfied and current state is
///                           still coherent, but a required element is no
///                           longer current or is unavailable.
///   REVALIDATION_REQUIRED - authoritative evidence was lost (process death,
///                           coordinator restart) and must be republished.
///   RETIRING              - an explicit retirement is in progress.
///   RETIRED               - no rack may be consumed from this cluster.
enum class ClusterLifecycle : std::uint8_t {
  Declared = 0,
  Forming = 1,
  Partial = 2,
  Ready = 3,
  Degraded = 4,
  RevalidationRequired = 5,
  Retiring = 6,
  Retired = 7,
};

[[nodiscard]] std::string_view to_string(ClusterLifecycle value) noexcept;
[[nodiscard]] std::optional<ClusterLifecycle> cluster_lifecycle_from_string(
    std::string_view text) noexcept;

/// True when consumers may treat the cluster composition as authoritative for
/// current use. PARTIAL and DEGRADED are explicit, not errors.
[[nodiscard]] constexpr bool is_consumable_lifecycle(ClusterLifecycle value) noexcept {
  return value == ClusterLifecycle::Ready || value == ClusterLifecycle::Partial ||
         value == ClusterLifecycle::Degraded;
}

/// True when the cluster may accept ordinary mutations.
[[nodiscard]] constexpr bool accepts_mutations(ClusterLifecycle value) noexcept {
  return value != ClusterLifecycle::Retired;
}

/// Cluster-side membership state of a rack.
enum class RackMembershipState : std::uint8_t {
  Unknown = 0,
  /// Accepted and part of the current cluster composition.
  Active = 1,
  /// Deliberately withdrawn from service; membership remains known and the
  /// rack is excluded from the current composition.
  Unavailable = 2,
  /// Removed by authority. A later publication must not resurrect it without
  /// an explicit re-admission.
  Withdrawn = 3,
  /// Permanently retired. The identity is remembered so it can never be
  /// silently resurrected.
  Retired = 4,
};

[[nodiscard]] std::string_view to_string(RackMembershipState value) noexcept;
[[nodiscard]] std::optional<RackMembershipState> rack_membership_state_from_string(
    std::string_view text) noexcept;

/// Lifecycle as reported by the rack authority (Rack Fabric). Cluster Fabric
/// relays it; it does not own it.
enum class RackLifecycleState : std::uint8_t {
  Unknown = 0,
  Declared = 1,
  Forming = 2,
  Ready = 3,
  Degraded = 4,
  Retiring = 5,
  Retired = 6,
};

[[nodiscard]] std::string_view to_string(RackLifecycleState value) noexcept;
[[nodiscard]] std::optional<RackLifecycleState> rack_lifecycle_state_from_string(
    std::string_view text) noexcept;

/// Whether the referenced rack generation is still the authoritative one.
enum class RackCurrentness : std::uint8_t {
  Unknown = 0,
  Current = 1,
  /// A newer RackGeneration supersedes the referenced one.
  Superseded = 2,
  /// The publishing authority is gone; the reference must be republished.
  RevalidationRequired = 3,
};

[[nodiscard]] std::string_view to_string(RackCurrentness value) noexcept;
[[nodiscard]] std::optional<RackCurrentness> rack_currentness_from_string(
    std::string_view text) noexcept;

/// The explicit readiness contract. Defaults are deliberately strict: a
/// cluster with no racks, no current rack and no topology evidence is not
/// READY.
struct ReadinessContract {
  /// Minimum number of ACTIVE racks with a CURRENT generation.
  std::size_t minimum_current_racks = 1;
  /// Racks that must be present, ACTIVE and CURRENT.
  std::vector<RackId> mandatory_racks;
  /// When true every ACTIVE rack must be CURRENT; any superseded or
  /// revalidation-required rack blocks READY.
  bool require_all_active_racks_current = true;
  /// When true at least one inter-rack link with current evidence is needed.
  bool require_connectivity_evidence = false;
  /// Minimum number of inter-rack links carrying current evidence.
  std::size_t minimum_current_links = 0;
  /// Placement-domain classes that must have at least one current domain.
  std::vector<PlacementDomainClass> required_placement_domain_classes;
  /// Failure-domain classes that must have at least one current domain.
  std::vector<FailureDomainClass> required_failure_domain_classes;
  /// When true, a cluster with a knowingly incomplete rack set may still be
  /// READY, and is reported as PARTIAL otherwise.
  bool allow_partial = true;
  /// When true, DEGRADED elements (unavailable racks that are not mandatory)
  /// do not block READY. When false any DEGRADED element blocks READY.
  bool allow_degraded = true;
  /// When true, unresolved structural conflicts (dangling references,
  /// conflicting publications) block READY.
  bool require_no_conflicts = true;

  [[nodiscard]] static ReadinessContract permissive() noexcept;
  [[nodiscard]] static ReadinessContract strict();

  friend bool operator==(const ReadinessContract&, const ReadinessContract&) = default;
};

/// One reason the readiness contract is not satisfied.
struct ReadinessBlocker {
  /// Stable machine-readable code, for example "mandatory_rack_missing".
  std::string code;
  /// Affected identity.
  std::string subject;
  /// Human-readable explanation.
  std::string detail;

  friend bool operator==(const ReadinessBlocker&, const ReadinessBlocker&) = default;
};

/// Deterministic evaluation result of the readiness contract.
struct ReadinessEvaluation {
  bool satisfied = false;
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;
  /// Sorted by (code, subject) so the result is deterministic.
  std::vector<ReadinessBlocker> blockers;

  [[nodiscard]] bool ok() const noexcept { return satisfied; }
};

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_LIFECYCLE_HPP
