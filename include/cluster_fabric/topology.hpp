// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Inter-rack connectivity and topology epochs.
//
// Cluster Fabric represents inter-rack relationships explicitly. It is not a
// route planner: it records what relationship was declared and observed, with
// provenance, and it never invents a bandwidth or a latency that was not
// supplied. Network reachability is not direct physical connectivity, and
// process connectivity to a coordinator is not proof of a data path.

#ifndef CLUSTER_FABRIC_TOPOLOGY_HPP
#define CLUSTER_FABRIC_TOPOLOGY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cluster_fabric/domains.hpp"
#include "cluster_fabric/evidence.hpp"
#include "cluster_fabric/generation.hpp"
#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/taxonomy.hpp"

namespace cluster_fabric {

/// One explicit inter-rack relationship. Direction is explicit: a
/// unidirectional relationship does not imply the reverse.
struct InterRackLink {
  InterRackLinkId id;
  RackId source;
  RackId destination;
  LinkDirection direction = LinkDirection::Unknown;
  ConnectivityClass connectivity = ConnectivityClass::Unknown;
  std::optional<NetworkDomainId> network_domain;
  std::optional<LinkDomainId> link_domain;
  std::optional<RackEndpointId> source_endpoint;
  std::optional<RackEndpointId> destination_endpoint;

  /// Coarse class supplied by the authority. UNKNOWN when not supplied.
  BandwidthClass bandwidth_class = BandwidthClass::Unknown;
  /// Exact nominal bandwidth, only when explicitly supplied.
  std::optional<std::uint64_t> nominal_bandwidth_bps;
  /// Coarse latency class supplied by the authority.
  LatencyClass latency_class = LatencyClass::Unknown;
  /// Exact nominal latency in nanoseconds, only when explicitly supplied.
  std::optional<std::uint64_t> nominal_latency_nanos;
  /// Hop or domain metadata when the authority supplies it.
  std::optional<std::uint32_t> hop_count;

  Reachability reachability = Reachability::Unknown;
  HealthState health = HealthState::Unknown;
  /// Failure domains this relationship belongs to. Sorted, deduplicated.
  std::vector<FailureDomainId> failure_domains;

  /// Evidence, generation, authority and label of this relationship.
  DomainHeader header;
  TopologyEpoch topology_epoch;
  TopologyGeneration topology_generation;
  PublicationGeneration publication;
  /// Process incarnation that authored the publication, when the publication
  /// came from a rack-side process.
  std::optional<RackAgentBootId> boot;

  friend bool operator==(const InterRackLink&, const InterRackLink&) = default;
};

/// One coherent authoritative cluster-topology regime.
struct TopologyEpochRecord {
  TopologyEpoch epoch;
  TopologyGeneration generation;
  Timestamp established_at = Timestamp::unknown();
  EvidenceStamp evidence;
  /// Stable reason code for the epoch change, for example "link_superseded".
  std::string reason;

  friend bool operator==(const TopologyEpochRecord&, const TopologyEpochRecord&) = default;
};

/// Result of comparing a topology epoch or generation against current state.
enum class TopologyCurrentness : std::uint8_t {
  Unknown = 0,
  Current = 1,
  StaleEpoch = 2,
  StaleGeneration = 3,
  RevalidationRequired = 4,
};

[[nodiscard]] std::string_view to_string(TopologyCurrentness value) noexcept;

/// Whether two racks are independent for a failure-domain class.
enum class DomainIndependence : std::uint8_t {
  /// Not enough current evidence to answer. Never silently Independent.
  Unknown = 0,
  Independent = 1,
  Shared = 2,
  RevalidationRequired = 3,
};

[[nodiscard]] std::string_view to_string(DomainIndependence value) noexcept;

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_TOPOLOGY_HPP
