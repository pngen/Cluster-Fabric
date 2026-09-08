// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The narrow rack contract consumed from Rack Fabric.
//
// Cluster Fabric composes the cluster from rack-level authorities. It does not
// duplicate rack-internal device inventories: a RackReference carries exactly
// the identity, generation, lifecycle, bounded composition summary, endpoints
// and failure-domain hints required to compose the cluster, plus the authority
// and provenance needed to decide whether the reference may be consumed.
//
// Rack Fabric owns everything inside a rack. Cluster Fabric owns the
// composition above it. The boundary is this type.

#ifndef CLUSTER_FABRIC_RACK_REFERENCE_HPP
#define CLUSTER_FABRIC_RACK_REFERENCE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "cluster_fabric/evidence.hpp"
#include "cluster_fabric/generation.hpp"
#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/lifecycle.hpp"
#include "cluster_fabric/taxonomy.hpp"

namespace cluster_fabric {

/// Bounded composition summary of one accelerator class inside a rack.
/// device_count is nullopt when the count is unknown: UNKNOWN never becomes
/// zero.
struct AcceleratorClassSummary {
  AcceleratorVendor vendor = AcceleratorVendor::Unknown;
  /// Bounded free-form family label, for example "H100". Empty when unknown.
  std::string family;
  std::optional<std::uint32_t> device_count;
  std::optional<std::uint64_t> device_memory_bytes;

  friend bool operator==(const AcceleratorClassSummary&, const AcceleratorClassSummary&) = default;
  friend auto operator<=>(const AcceleratorClassSummary&, const AcceleratorClassSummary&) = default;
};

/// Bounded composition summary of a rack. Every numeric field is optional;
/// an absent value means UNKNOWN and is never coerced to zero.
struct RackCompositionSummary {
  /// Sorted by (vendor, family) for deterministic output.
  std::vector<AcceleratorClassSummary> accelerators;
  std::optional<std::uint32_t> cpu_sockets;
  std::optional<std::uint32_t> cpu_cores;
  std::optional<std::uint64_t> host_memory_bytes;
  std::optional<std::uint32_t> nic_count;
  std::optional<std::uint32_t> switch_count;
  /// Operator-supplied composition class label, for example "gpu-dense".
  std::string composition_label;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;

  [[nodiscard]] bool empty() const noexcept {
    return accelerators.empty() && !cpu_sockets.has_value() && !cpu_cores.has_value() &&
           !host_memory_bytes.has_value() && !nic_count.has_value() && !switch_count.has_value() &&
           composition_label.empty();
  }

  friend bool operator==(const RackCompositionSummary&, const RackCompositionSummary&) = default;
};

/// One rack endpoint relevant to inter-rack connectivity. Cluster Fabric does
/// not inventory the device behind the endpoint.
struct RackEndpoint {
  RackEndpointId id;
  std::optional<NetworkDomainId> network_domain;
  std::optional<LinkDomainId> link_domain;
  ConnectivityClass connectivity = ConnectivityClass::Unknown;
  EvidenceStamp evidence;

  friend bool operator==(const RackEndpoint&, const RackEndpoint&) = default;
};

/// A rack-level failure-domain hint. Cluster Fabric maps hints onto cluster
/// level failure domains; the rack authority remains the source of the hint.
struct RackFailureDomainHint {
  FailureDomainClass klass = FailureDomainClass::Unknown;
  FailureDomainId id;
  EvidenceStamp evidence;

  friend bool operator==(const RackFailureDomainHint&, const RackFailureDomainHint&) = default;
};

/// The complete, narrow reference Cluster Fabric accepts about one rack.
struct RackReference {
  RackId rack;
  /// Authoritative generation of the rack's composition, owned by Rack Fabric.
  RackGeneration generation;
  RackLifecycleState rack_lifecycle = RackLifecycleState::Unknown;
  RackCurrentness currentness = RackCurrentness::Unknown;
  RackCompositionSummary composition;
  /// Sorted by endpoint id, deduplicated.
  std::vector<RackEndpoint> endpoints;
  /// Sorted by (class, id), deduplicated.
  std::vector<RackFailureDomainHint> failure_domain_hints;

  /// Provenance and freshness of the reference as a whole.
  EvidenceStamp evidence;
  /// Durable authority entitled to publish this rack.
  std::optional<RackPublisherId> publisher;
  /// Monotonic per-publisher publication counter.
  PublicationGeneration publication;
  /// Process incarnation that authored this publication.
  RackAgentBootId boot;
  /// Cluster epoch and coordinator epoch the publisher believed current.
  ClusterEpoch cluster_epoch;
  CoordinatorEpoch coordinator_epoch;
  /// Rack-level health as reported by the rack authority.
  HealthState health = HealthState::Unknown;
  /// Origin label, for example "rack-fabric:1.0.0".
  std::string origin_label;

  friend bool operator==(const RackReference&, const RackReference&) = default;
};

enum class RackReferenceIssue : std::uint8_t {
  None = 0,
  MissingRackId,
  MissingGeneration,
  TooManyEndpoints,
  TooManyFailureDomainHints,
  TooManyAcceleratorClasses,
  InvalidLabel,
  DuplicateEndpoint,
  DuplicateFailureDomainHint,
  MissingPublisher,
  UnknownProvenance,
};

[[nodiscard]] std::string_view to_string(RackReferenceIssue value) noexcept;

struct RackReferenceValidation {
  RackReferenceIssue issue = RackReferenceIssue::None;
  std::string subject;
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return issue == RackReferenceIssue::None; }
};

/// Validates a reference received from a peer or decoded from a file. Bounds
/// are checked before the reference is used for anything.
[[nodiscard]] RackReferenceValidation validate_rack_reference(const RackReference& reference);

/// Returns a canonical copy: racks lists sorted and deduplicated, accelerator
/// classes ordered. Used before the reference enters canonical state.
[[nodiscard]] RackReference canonicalize(RackReference reference);

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_RACK_REFERENCE_HPP
