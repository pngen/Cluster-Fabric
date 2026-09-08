// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Canonical spellings and parsing for every classification taxonomy.
//
// Canonical text is the SCREAMING_SNAKE_CASE of the enumerator. Parsing also
// accepts the exact enumerator spelling. The UNKNOWN member is the single
// exception: it is produced and accepted only as "UNKNOWN", because every
// taxonomy reserves it for absence of classification rather than a value a
// caller may spell loosely.

#include "cluster_fabric/taxonomy.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace cluster_fabric {
namespace {

template <class Enum>
struct EnumName {
  Enum value;
  std::string_view canonical;
  std::string_view enumerator;
};

constexpr std::array<EnumName<FailureDomainClass>, 10> kFailureDomainClassNames{{
    {FailureDomainClass::Unknown, "UNKNOWN", "Unknown"},
    {FailureDomainClass::Rack, "RACK", "Rack"},
    {FailureDomainClass::Row, "ROW", "Row"},
    {FailureDomainClass::SwitchPlane, "SWITCH_PLANE", "SwitchPlane"},
    {FailureDomainClass::PowerFeed, "POWER_FEED", "PowerFeed"},
    {FailureDomainClass::NetworkPlane, "NETWORK_PLANE", "NetworkPlane"},
    {FailureDomainClass::AvailabilityZone, "AVAILABILITY_ZONE", "AvailabilityZone"},
    {FailureDomainClass::StorageDomain, "STORAGE_DOMAIN", "StorageDomain"},
    {FailureDomainClass::CoolingLoop, "COOLING_LOOP", "CoolingLoop"},
    {FailureDomainClass::ControlPlane, "CONTROL_PLANE", "ControlPlane"},
}};

constexpr std::array<EnumName<PlacementDomainClass>, 8> kPlacementDomainClassNames{{
    {PlacementDomainClass::Unknown, "UNKNOWN", "Unknown"},
    {PlacementDomainClass::LowLatencyFabric, "LOW_LATENCY_FABRIC", "LowLatencyFabric"},
    {PlacementDomainClass::AvailabilityDomain, "AVAILABILITY_DOMAIN", "AvailabilityDomain"},
    {PlacementDomainClass::NetworkTier, "NETWORK_TIER", "NetworkTier"},
    {PlacementDomainClass::PowerBoundary, "POWER_BOUNDARY", "PowerBoundary"},
    {PlacementDomainClass::StorageLocality, "STORAGE_LOCALITY", "StorageLocality"},
    {PlacementDomainClass::FailureDomain, "FAILURE_DOMAIN", "FailureDomain"},
    {PlacementDomainClass::PolicyClass, "POLICY_CLASS", "PolicyClass"},
}};

constexpr std::array<EnumName<CapacityDomainClass>, 6> kCapacityDomainClassNames{{
    {CapacityDomainClass::Unknown, "UNKNOWN", "Unknown"},
    {CapacityDomainClass::RackGroup, "RACK_GROUP", "RackGroup"},
    {CapacityDomainClass::AcceleratorPool, "ACCELERATOR_POOL", "AcceleratorPool"},
    {CapacityDomainClass::CpuPool, "CPU_POOL", "CpuPool"},
    {CapacityDomainClass::NetworkDomain, "NETWORK_DOMAIN", "NetworkDomain"},
    {CapacityDomainClass::StorageDomain, "STORAGE_DOMAIN", "StorageDomain"},
}};

constexpr std::array<EnumName<ConnectivityClass>, 6> kConnectivityClassNames{{
    {ConnectivityClass::Unknown, "UNKNOWN", "Unknown"},
    {ConnectivityClass::DirectFabric, "DIRECT_FABRIC", "DirectFabric"},
    {ConnectivityClass::SwitchedFabric, "SWITCHED_FABRIC", "SwitchedFabric"},
    {ConnectivityClass::RoutedPath, "ROUTED_PATH", "RoutedPath"},
    {ConnectivityClass::Overlay, "OVERLAY", "Overlay"},
    {ConnectivityClass::ManagementOnly, "MANAGEMENT_ONLY", "ManagementOnly"},
}};

constexpr std::array<EnumName<LinkDirection>, 3> kLinkDirectionNames{{
    {LinkDirection::Unknown, "UNKNOWN", "Unknown"},
    {LinkDirection::Unidirectional, "UNIDIRECTIONAL", "Unidirectional"},
    {LinkDirection::Bidirectional, "BIDIRECTIONAL", "Bidirectional"},
}};

constexpr std::array<EnumName<Reachability>, 4> kReachabilityNames{{
    {Reachability::Unknown, "UNKNOWN", "Unknown"},
    {Reachability::Reachable, "REACHABLE", "Reachable"},
    {Reachability::Unreachable, "UNREACHABLE", "Unreachable"},
    {Reachability::RevalidationRequired, "REVALIDATION_REQUIRED", "RevalidationRequired"},
}};

constexpr std::array<EnumName<BandwidthClass>, 5> kBandwidthClassNames{{
    {BandwidthClass::Unknown, "UNKNOWN", "Unknown"},
    {BandwidthClass::Low, "LOW", "Low"},
    {BandwidthClass::Moderate, "MODERATE", "Moderate"},
    {BandwidthClass::High, "HIGH", "High"},
    {BandwidthClass::VeryHigh, "VERY_HIGH", "VeryHigh"},
}};

constexpr std::array<EnumName<LatencyClass>, 5> kLatencyClassNames{{
    {LatencyClass::Unknown, "UNKNOWN", "Unknown"},
    {LatencyClass::VeryLow, "VERY_LOW", "VeryLow"},
    {LatencyClass::Low, "LOW", "Low"},
    {LatencyClass::Moderate, "MODERATE", "Moderate"},
    {LatencyClass::High, "HIGH", "High"},
}};

constexpr std::array<EnumName<AcceleratorVendor>, 5> kAcceleratorVendorNames{{
    {AcceleratorVendor::Unknown, "UNKNOWN", "Unknown"},
    {AcceleratorVendor::Nvidia, "NVIDIA", "Nvidia"},
    {AcceleratorVendor::Amd, "AMD", "Amd"},
    {AcceleratorVendor::Intel, "INTEL", "Intel"},
    {AcceleratorVendor::Other, "OTHER", "Other"},
}};

constexpr std::array<EnumName<HealthState>, 4> kHealthStateNames{{
    {HealthState::Unknown, "UNKNOWN", "Unknown"},
    {HealthState::Healthy, "HEALTHY", "Healthy"},
    {HealthState::Degraded, "DEGRADED", "Degraded"},
    {HealthState::Unhealthy, "UNHEALTHY", "Unhealthy"},
}};

constexpr std::array<EnumName<ConstraintKind>, 6> kConstraintKindNames{{
    {ConstraintKind::Unknown, "UNKNOWN", "Unknown"},
    {ConstraintKind::PlacementScope, "PLACEMENT_SCOPE", "PlacementScope"},
    {ConstraintKind::ConnectivityRequirement, "CONNECTIVITY_REQUIREMENT", "ConnectivityRequirement"},
    {ConstraintKind::FailureDomainIndependence, "FAILURE_DOMAIN_INDEPENDENCE",
     "FailureDomainIndependence"},
    {ConstraintKind::CapacityFloor, "CAPACITY_FLOOR", "CapacityFloor"},
    {ConstraintKind::MaintenanceWindow, "MAINTENANCE_WINDOW", "MaintenanceWindow"},
}};

template <class Enum, std::size_t N>
[[nodiscard]] std::string_view render(Enum value,
                                      const std::array<EnumName<Enum>, N>& names) noexcept {
  for (const EnumName<Enum>& name : names) {
    if (name.value == value) {
      return name.canonical;
    }
  }
  return "UNKNOWN";
}

template <class Enum, std::size_t N>
[[nodiscard]] std::optional<Enum> parse(std::string_view text,
                                        const std::array<EnumName<Enum>, N>& names) noexcept {
  for (const EnumName<Enum>& name : names) {
    if (text == name.canonical) {
      return name.value;
    }
    if (name.value != Enum::Unknown && text == name.enumerator) {
      return name.value;
    }
  }
  return std::nullopt;
}

}  // namespace

std::string_view to_string(FailureDomainClass value) noexcept {
  return render(value, kFailureDomainClassNames);
}

std::string_view to_string(PlacementDomainClass value) noexcept {
  return render(value, kPlacementDomainClassNames);
}

std::string_view to_string(CapacityDomainClass value) noexcept {
  return render(value, kCapacityDomainClassNames);
}

std::string_view to_string(ConnectivityClass value) noexcept {
  return render(value, kConnectivityClassNames);
}

std::string_view to_string(LinkDirection value) noexcept {
  return render(value, kLinkDirectionNames);
}

std::string_view to_string(Reachability value) noexcept {
  return render(value, kReachabilityNames);
}

std::string_view to_string(BandwidthClass value) noexcept {
  return render(value, kBandwidthClassNames);
}

std::string_view to_string(LatencyClass value) noexcept {
  return render(value, kLatencyClassNames);
}

std::string_view to_string(AcceleratorVendor value) noexcept {
  return render(value, kAcceleratorVendorNames);
}

std::string_view to_string(HealthState value) noexcept {
  return render(value, kHealthStateNames);
}

std::string_view to_string(ConstraintKind value) noexcept {
  return render(value, kConstraintKindNames);
}

std::optional<FailureDomainClass> failure_domain_class_from_string(std::string_view text) noexcept {
  return parse(text, kFailureDomainClassNames);
}

std::optional<PlacementDomainClass> placement_domain_class_from_string(
    std::string_view text) noexcept {
  return parse(text, kPlacementDomainClassNames);
}

std::optional<CapacityDomainClass> capacity_domain_class_from_string(
    std::string_view text) noexcept {
  return parse(text, kCapacityDomainClassNames);
}

std::optional<ConnectivityClass> connectivity_class_from_string(std::string_view text) noexcept {
  return parse(text, kConnectivityClassNames);
}

std::optional<LinkDirection> link_direction_from_string(std::string_view text) noexcept {
  return parse(text, kLinkDirectionNames);
}

std::optional<Reachability> reachability_from_string(std::string_view text) noexcept {
  return parse(text, kReachabilityNames);
}

std::optional<BandwidthClass> bandwidth_class_from_string(std::string_view text) noexcept {
  return parse(text, kBandwidthClassNames);
}

std::optional<LatencyClass> latency_class_from_string(std::string_view text) noexcept {
  return parse(text, kLatencyClassNames);
}

std::optional<AcceleratorVendor> accelerator_vendor_from_string(std::string_view text) noexcept {
  return parse(text, kAcceleratorVendorNames);
}

std::optional<HealthState> health_state_from_string(std::string_view text) noexcept {
  return parse(text, kHealthStateNames);
}

std::optional<ConstraintKind> constraint_kind_from_string(std::string_view text) noexcept {
  return parse(text, kConstraintKindNames);
}

}  // namespace cluster_fabric
