// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Lifecycle spellings and the explicit readiness contracts.
//
// The two named contracts are fixed points of the configuration space: the
// permissive contract is used by exploratory and synthetic topologies, the
// strict contract by production clusters. Neither is inferred; both are
// returned explicitly so a caller never has to guess a default.

#include "cluster_fabric/lifecycle.hpp"

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

constexpr std::array<EnumName<ClusterLifecycle>, 8> kClusterLifecycleNames{{
    {ClusterLifecycle::Declared, "DECLARED", "Declared"},
    {ClusterLifecycle::Forming, "FORMING", "Forming"},
    {ClusterLifecycle::Partial, "PARTIAL", "Partial"},
    {ClusterLifecycle::Ready, "READY", "Ready"},
    {ClusterLifecycle::Degraded, "DEGRADED", "Degraded"},
    {ClusterLifecycle::RevalidationRequired, "REVALIDATION_REQUIRED", "RevalidationRequired"},
    {ClusterLifecycle::Retiring, "RETIRING", "Retiring"},
    {ClusterLifecycle::Retired, "RETIRED", "Retired"},
}};

constexpr std::array<EnumName<RackMembershipState>, 5> kRackMembershipStateNames{{
    {RackMembershipState::Unknown, "UNKNOWN", "Unknown"},
    {RackMembershipState::Active, "ACTIVE", "Active"},
    {RackMembershipState::Unavailable, "UNAVAILABLE", "Unavailable"},
    {RackMembershipState::Withdrawn, "WITHDRAWN", "Withdrawn"},
    {RackMembershipState::Retired, "RETIRED", "Retired"},
}};

constexpr std::array<EnumName<RackLifecycleState>, 7> kRackLifecycleStateNames{{
    {RackLifecycleState::Unknown, "UNKNOWN", "Unknown"},
    {RackLifecycleState::Declared, "DECLARED", "Declared"},
    {RackLifecycleState::Forming, "FORMING", "Forming"},
    {RackLifecycleState::Ready, "READY", "Ready"},
    {RackLifecycleState::Degraded, "DEGRADED", "Degraded"},
    {RackLifecycleState::Retiring, "RETIRING", "Retiring"},
    {RackLifecycleState::Retired, "RETIRED", "Retired"},
}};

constexpr std::array<EnumName<RackCurrentness>, 4> kRackCurrentnessNames{{
    {RackCurrentness::Unknown, "UNKNOWN", "Unknown"},
    {RackCurrentness::Current, "CURRENT", "Current"},
    {RackCurrentness::Superseded, "SUPERSEDED", "Superseded"},
    {RackCurrentness::RevalidationRequired, "REVALIDATION_REQUIRED", "RevalidationRequired"},
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
    if (text == name.canonical || text == name.enumerator) {
      return name.value;
    }
  }
  return std::nullopt;
}

}  // namespace

std::string_view to_string(ClusterLifecycle value) noexcept {
  return render(value, kClusterLifecycleNames);
}

std::optional<ClusterLifecycle> cluster_lifecycle_from_string(std::string_view text) noexcept {
  return parse(text, kClusterLifecycleNames);
}

std::string_view to_string(RackMembershipState value) noexcept {
  return render(value, kRackMembershipStateNames);
}

std::optional<RackMembershipState> rack_membership_state_from_string(
    std::string_view text) noexcept {
  return parse(text, kRackMembershipStateNames);
}

std::string_view to_string(RackLifecycleState value) noexcept {
  return render(value, kRackLifecycleStateNames);
}

std::optional<RackLifecycleState> rack_lifecycle_state_from_string(std::string_view text) noexcept {
  return parse(text, kRackLifecycleStateNames);
}

std::string_view to_string(RackCurrentness value) noexcept {
  return render(value, kRackCurrentnessNames);
}

std::optional<RackCurrentness> rack_currentness_from_string(std::string_view text) noexcept {
  return parse(text, kRackCurrentnessNames);
}

ReadinessContract ReadinessContract::permissive() noexcept {
  ReadinessContract contract;
  contract.minimum_current_racks = 0;
  contract.require_all_active_racks_current = false;
  contract.require_connectivity_evidence = false;
  contract.minimum_current_links = 0;
  contract.allow_partial = true;
  contract.allow_degraded = true;
  contract.require_no_conflicts = false;
  return contract;
}

ReadinessContract ReadinessContract::strict() {
  ReadinessContract contract;
  contract.minimum_current_racks = 1;
  contract.require_all_active_racks_current = true;
  contract.require_connectivity_evidence = true;
  contract.minimum_current_links = 1;
  contract.allow_partial = false;
  contract.allow_degraded = false;
  contract.require_no_conflicts = true;
  return contract;
}

}  // namespace cluster_fabric
