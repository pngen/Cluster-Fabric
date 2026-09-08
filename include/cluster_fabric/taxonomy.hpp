// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Classification taxonomies.
//
// These enums describe infrastructure structure. They are deliberately
// descriptive: Cluster Fabric records what the infrastructure is, never what a
// higher-level runtime should do about it. Every enum reserves an UNKNOWN
// member and never invents a positive classification.

#ifndef CLUSTER_FABRIC_TAXONOMY_HPP
#define CLUSTER_FABRIC_TAXONOMY_HPP

#include <cstdint>
#include <optional>
#include <string_view>

namespace cluster_fabric {

/// Failure-domain classes above and including the rack. Overlapping domains
/// are legitimate: a rack may sit in a row, a power feed and a network plane
/// at the same time.
enum class FailureDomainClass : std::uint8_t {
  Unknown = 0,
  Rack = 1,
  Row = 2,
  SwitchPlane = 3,
  PowerFeed = 4,
  NetworkPlane = 5,
  AvailabilityZone = 6,
  StorageDomain = 7,
  CoolingLoop = 8,
  ControlPlane = 9,
};

/// Placement-domain classes. Descriptive infrastructure groupings only.
enum class PlacementDomainClass : std::uint8_t {
  Unknown = 0,
  LowLatencyFabric = 1,
  AvailabilityDomain = 2,
  NetworkTier = 3,
  PowerBoundary = 4,
  StorageLocality = 5,
  FailureDomain = 6,
  PolicyClass = 7,
};

/// Capacity-domain classes. Structural aggregation scopes, never grants.
enum class CapacityDomainClass : std::uint8_t {
  Unknown = 0,
  RackGroup = 1,
  AcceleratorPool = 2,
  CpuPool = 3,
  NetworkDomain = 4,
  StorageDomain = 5,
};

/// Inter-rack connectivity classes. A routed path is not a direct fabric
/// attachment, and management-only reachability is not a data path.
enum class ConnectivityClass : std::uint8_t {
  Unknown = 0,
  DirectFabric = 1,
  SwitchedFabric = 2,
  RoutedPath = 3,
  Overlay = 4,
  ManagementOnly = 5,
};

enum class LinkDirection : std::uint8_t {
  Unknown = 0,
  Unidirectional = 1,
  Bidirectional = 2,
};

/// Explicit reachability. A stale successful probe is never current proof.
enum class Reachability : std::uint8_t {
  Unknown = 0,
  Reachable = 1,
  Unreachable = 2,
  RevalidationRequired = 3,
};

/// Coarse bandwidth class used when an exact nominal value is not supplied.
enum class BandwidthClass : std::uint8_t {
  Unknown = 0,
  Low = 1,
  Moderate = 2,
  High = 3,
  VeryHigh = 4,
};

/// Coarse latency class used when an exact measurement is not supplied.
enum class LatencyClass : std::uint8_t {
  Unknown = 0,
  VeryLow = 1,
  Low = 2,
  Moderate = 3,
  High = 4,
};

/// Accelerator vendor class for heterogeneous composition summaries. Cluster
/// Fabric records the vendor; it never dispatches to it.
enum class AcceleratorVendor : std::uint8_t {
  Unknown = 0,
  Nvidia = 1,
  Amd = 2,
  Intel = 3,
  Other = 4,
};

enum class HealthState : std::uint8_t {
  Unknown = 0,
  Healthy = 1,
  Degraded = 2,
  Unhealthy = 3,
};

/// Kinds of cluster-level constraint evidence. These records describe
/// requirements the infrastructure was declared to satisfy. Cluster Fabric
/// publishes them; it does not enforce placement.
enum class ConstraintKind : std::uint8_t {
  Unknown = 0,
  PlacementScope = 1,
  ConnectivityRequirement = 2,
  FailureDomainIndependence = 3,
  CapacityFloor = 4,
  MaintenanceWindow = 5,
};

[[nodiscard]] std::string_view to_string(FailureDomainClass value) noexcept;
[[nodiscard]] std::string_view to_string(PlacementDomainClass value) noexcept;
[[nodiscard]] std::string_view to_string(CapacityDomainClass value) noexcept;
[[nodiscard]] std::string_view to_string(ConnectivityClass value) noexcept;
[[nodiscard]] std::string_view to_string(LinkDirection value) noexcept;
[[nodiscard]] std::string_view to_string(Reachability value) noexcept;
[[nodiscard]] std::string_view to_string(BandwidthClass value) noexcept;
[[nodiscard]] std::string_view to_string(LatencyClass value) noexcept;
[[nodiscard]] std::string_view to_string(AcceleratorVendor value) noexcept;
[[nodiscard]] std::string_view to_string(HealthState value) noexcept;
[[nodiscard]] std::string_view to_string(ConstraintKind value) noexcept;

[[nodiscard]] std::optional<FailureDomainClass> failure_domain_class_from_string(
    std::string_view text) noexcept;
[[nodiscard]] std::optional<PlacementDomainClass> placement_domain_class_from_string(
    std::string_view text) noexcept;
[[nodiscard]] std::optional<CapacityDomainClass> capacity_domain_class_from_string(
    std::string_view text) noexcept;
[[nodiscard]] std::optional<ConnectivityClass> connectivity_class_from_string(
    std::string_view text) noexcept;
[[nodiscard]] std::optional<LinkDirection> link_direction_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<Reachability> reachability_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<BandwidthClass> bandwidth_class_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<LatencyClass> latency_class_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<AcceleratorVendor> accelerator_vendor_from_string(
    std::string_view text) noexcept;
[[nodiscard]] std::optional<HealthState> health_state_from_string(std::string_view text) noexcept;
[[nodiscard]] std::optional<ConstraintKind> constraint_kind_from_string(std::string_view text) noexcept;

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_TAXONOMY_HPP
