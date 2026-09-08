// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic synthetic cluster laboratory.
//
// The generator produces reproducible multi-rack cluster scenarios for state
// machine, invariant and architectural proof. Every rack it invents is
// SYNTHETIC and must never be presented as physical infrastructure. On failure
// the generator emits the exact parameters and seed needed to reproduce the
// scenario.

#ifndef CLUSTER_FABRIC_SYNTHETIC_HPP
#define CLUSTER_FABRIC_SYNTHETIC_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cluster_fabric/cluster_state.hpp"
#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/mutation.hpp"
#include "cluster_fabric/rack_reference.hpp"

namespace cluster_fabric {

struct SyntheticConfig {
  std::uint64_t seed = 1;
  ClusterId cluster;
  RackPublisherId publisher;
  RackAgentBootId boot;

  std::size_t rack_count = 8;
  std::size_t racks_per_placement_domain = 2;
  std::size_t racks_per_failure_domain = 2;
  std::size_t network_domain_count = 2;
  /// Fraction of possible rack pairs that receive a link, in [0, 1].
  double link_density = 0.35;
  /// Produce racks with different accelerator vendors and device counts.
  bool heterogeneous = true;
  /// Racks deliberately omitted from the generated composition.
  std::size_t missing_rack_count = 0;
  /// Racks published with superseded generations.
  std::size_t stale_rack_count = 0;
  /// Racks whose generation is advanced after initial publication.
  std::size_t rack_generation_changes = 0;
  /// Placement domains deliberately left with incomplete membership.
  std::size_t degraded_domain_count = 0;
  /// Racks deliberately partitioned from the rest (no links).
  std::size_t partition_count = 0;
  /// Emit unidirectional links for a subset of pairs.
  bool asymmetric_connectivity = false;
  /// Topology-epoch changes applied after the initial composition.
  std::size_t topology_epoch_changes = 0;
  /// Emit power and cooling hierarchies above the racks.
  bool include_power_cooling = false;
  /// Rack indices that must be present; empty means all generated racks.
  std::vector<std::size_t> mandatory_rack_indices;

  [[nodiscard]] std::string reproduction_parameters() const;
  [[nodiscard]] std::uint64_t effective_seed() const noexcept { return seed; }
};

struct SyntheticCluster {
  SyntheticConfig config;
  ClusterId cluster;
  /// Rack identities in generation order.
  std::vector<RackId> racks;
  /// Mutation sequence that builds the scenario in order.
  std::vector<MutationRequest> requests;
  /// Rack references that were deliberately left stale.
  std::vector<RackId> stale_racks;
  /// Rack identities deliberately omitted.
  std::vector<RackId> missing_racks;
  /// Failure domain identities by class.
  std::vector<FailureDomain> failure_domains;
  std::vector<PlacementDomain> placement_domains;
  std::vector<CapacityDomain> capacity_domains;
  std::vector<InterRackLink> links;

  [[nodiscard]] std::string reproduction_parameters() const {
    return config.reproduction_parameters();
  }
};

/// Generates a deterministic scenario. Identical configuration always yields
/// byte-identical requests.
[[nodiscard]] SyntheticCluster generate_synthetic_cluster(const SyntheticConfig& config);

/// Canonical rack identity for a synthetic index.
[[nodiscard]] RackId synthetic_rack_id(std::size_t index);

/// Canonical domain identity helpers.
[[nodiscard]] PlacementDomainId synthetic_placement_domain_id(std::size_t index);
[[nodiscard]] FailureDomainId synthetic_failure_domain_id(FailureDomainClass klass,
                                                         std::size_t index);
[[nodiscard]] CapacityDomainId synthetic_capacity_domain_id(std::size_t index);
[[nodiscard]] NetworkDomainId synthetic_network_domain_id(std::size_t index);
[[nodiscard]] InterRackLinkId synthetic_link_id(const RackId& source, const RackId& destination);

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_SYNTHETIC_HPP
