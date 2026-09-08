// Copyright 2026 Sunny Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Snapshot staleness: one concrete scenario for every SnapshotStaleReason,
// semantic-digest stability and coverage, rack-generation bindings, lifecycle
// consumability, and validation against an advanced coordinator.

#include "harness.hpp"

#include "cluster_fabric/cluster_fabric.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cluster_fabric;

namespace {

template <class Id>
Id make_id(std::string_view text) {
  const std::optional<Id> parsed = Id::parse(text);
  CF_EXPECT(parsed.has_value());
  return *parsed;
}

EvidenceStamp current_stamp(std::int64_t millis) {
  EvidenceStamp stamp;
  stamp.provenance = EvidenceProvenance::Measured;
  stamp.observed_at = Timestamp::from_unix_millis(millis);
  stamp.ttl_millis = 60'000;
  stamp.freshness = Freshness::Fresh;
  return stamp;
}

DomainHeader domain_header(std::int64_t millis, std::uint64_t generation, const char* label) {
  DomainHeader header;
  header.evidence = current_stamp(millis);
  header.generation = DomainGeneration::from_raw(generation);
  header.publisher = make_id<RackPublisherId>("publisher-a");
  header.cluster_epoch = ClusterEpoch::from_raw(1);
  header.coordinator_epoch = CoordinatorEpoch::from_raw(1);
  header.label = label;
  return header;
}

RackRecord rack_record(const char* rack, const char* boot, std::uint64_t generation,
                       RackMembershipState membership, bool authoritative) {
  RackRecord record;
  record.reference.rack = make_id<RackId>(rack);
  record.reference.generation = RackGeneration::from_raw(generation);
  record.reference.rack_lifecycle = RackLifecycleState::Ready;
  record.reference.currentness = RackCurrentness::Current;
  record.reference.health = HealthState::Healthy;
  record.reference.evidence = current_stamp(1'000'000);
  record.reference.publisher = make_id<RackPublisherId>("publisher-a");
  record.reference.publication = PublicationGeneration::from_raw(2);
  record.reference.boot = make_id<RackAgentBootId>(boot);
  record.reference.cluster_epoch = ClusterEpoch::from_raw(1);
  record.reference.coordinator_epoch = CoordinatorEpoch::from_raw(1);
  record.reference.origin_label = "rack-fabric";
  AcceleratorClassSummary accelerator;
  accelerator.vendor = AcceleratorVendor::Nvidia;
  accelerator.family = "H100";
  accelerator.device_count = std::uint32_t{8};
  accelerator.device_memory_bytes = std::uint64_t{80ull * 1024ull * 1024ull * 1024ull};
  record.reference.composition.accelerators = {accelerator};
  record.reference.composition.composition_label = "gpu-dense";
  record.reference.composition.provenance = EvidenceProvenance::Measured;
  RackEndpoint endpoint;
  endpoint.id = make_id<RackEndpointId>("endpoint-0");
  endpoint.network_domain = make_id<NetworkDomainId>("network-a");
  endpoint.link_domain = make_id<LinkDomainId>("link-domain-a");
  endpoint.connectivity = ConnectivityClass::DirectFabric;
  endpoint.evidence = current_stamp(1'000'000);
  record.reference.endpoints = {endpoint};
  RackFailureDomainHint hint;
  hint.klass = FailureDomainClass::Row;
  hint.id = make_id<FailureDomainId>("row-1");
  hint.evidence = current_stamp(1'000'000);
  record.reference.failure_domain_hints = {hint};
  record.membership = membership;
  record.membership_generation = MembershipGeneration::from_raw(5);
  record.accepted_at_generation = ClusterGeneration::from_raw(5);
  record.membership_evidence = current_stamp(1'000'000);
  record.authoritative_current = authoritative;
  record.non_authoritative_reason =
      authoritative ? std::string() : std::string("evidence_not_current");
  return record;
}

InterRackLink link_record() {
  InterRackLink link;
  link.id = make_id<InterRackLinkId>("link-a");
  link.source = make_id<RackId>("rack-01");
  link.destination = make_id<RackId>("rack-02");
  link.direction = LinkDirection::Bidirectional;
  link.connectivity = ConnectivityClass::DirectFabric;
  link.network_domain = make_id<NetworkDomainId>("network-a");
  link.link_domain = make_id<LinkDomainId>("link-domain-a");
  link.source_endpoint = make_id<RackEndpointId>("endpoint-0");
  link.destination_endpoint = make_id<RackEndpointId>("endpoint-1");
  link.bandwidth_class = BandwidthClass::VeryHigh;
  link.nominal_bandwidth_bps = std::uint64_t{400ull * 1000ull * 1000ull * 1000ull};
  link.latency_class = LatencyClass::VeryLow;
  link.nominal_latency_nanos = std::uint64_t{900};
  link.hop_count = std::uint32_t{1};
  link.reachability = Reachability::Reachable;
  link.health = HealthState::Healthy;
  link.failure_domains = {make_id<FailureDomainId>("row-1")};
  link.header = domain_header(1'000'000, 3, "row-a");
  link.topology_epoch = TopologyEpoch::from_raw(2);
  link.topology_generation = TopologyGeneration::from_raw(3);
  link.publication = PublicationGeneration::from_raw(2);
  link.boot = make_id<RackAgentBootId>("boot-01");
  return link;
}

/// A fully populated canonical state that validates as current against itself.
ClusterState rich_state() {
  ClusterState state;
  state.id = make_id<ClusterId>("cluster-a");
  state.epoch = ClusterEpoch::from_raw(1);
  state.coordinator_epoch = CoordinatorEpoch::from_raw(1);
  state.generation = ClusterGeneration::from_raw(5);
  state.membership_generation = MembershipGeneration::from_raw(5);
  state.topology_epoch = TopologyEpoch::from_raw(2);
  state.topology_generation = TopologyGeneration::from_raw(3);
  state.connectivity_generation = ConnectivityGeneration::from_raw(4);
  state.health_generation = HealthGeneration::from_raw(4);
  state.constraint_generation = ConstraintGeneration::from_raw(4);
  state.domain_generations.placement = DomainGeneration::from_raw(3);
  state.domain_generations.capacity = DomainGeneration::from_raw(3);
  state.domain_generations.failure = DomainGeneration::from_raw(3);
  state.domain_generations.network = DomainGeneration::from_raw(3);
  state.domain_generations.storage = DomainGeneration::from_raw(3);
  state.domain_generations.power = DomainGeneration::from_raw(3);
  state.domain_generations.cooling = DomainGeneration::from_raw(3);
  state.domain_generations.link = DomainGeneration::from_raw(3);
  state.snapshot_generation = SnapshotGeneration::from_raw(2);
  state.publication_generation = PublicationGeneration::from_raw(2);
  state.lifecycle = ClusterLifecycle::Ready;
  state.readiness_contract = ReadinessContract::permissive();
  state.topology_record.epoch = state.topology_epoch;
  state.topology_record.generation = state.topology_generation;
  state.topology_record.established_at = Timestamp::from_unix_millis(1'000'000);
  state.topology_record.evidence = current_stamp(1'000'000);
  state.topology_record.reason = "cluster_declared";
  state.declared_at = Timestamp::from_unix_millis(1'000'000);
  state.last_mutation_at = Timestamp::from_unix_millis(1'000'000);

  state.racks.emplace(make_id<RackId>("rack-01"),
                      rack_record("rack-01", "boot-01", 7, RackMembershipState::Active, true));
  state.racks.emplace(make_id<RackId>("rack-02"),
                      rack_record("rack-02", "boot-02", 3, RackMembershipState::Active, true));
  state.links.emplace(make_id<InterRackLinkId>("link-a"), link_record());

  PlacementDomain placement;
  placement.id = make_id<PlacementDomainId>("placement-a");
  placement.klass = PlacementDomainClass::LowLatencyFabric;
  placement.racks = {make_id<RackId>("rack-01"), make_id<RackId>("rack-02")};
  placement.header = domain_header(1'000'000, 3, "placement");
  state.placement_domains.emplace(placement.id, placement);

  CapacityDomain capacity;
  capacity.id = make_id<CapacityDomainId>("capacity-a");
  capacity.klass = CapacityDomainClass::AcceleratorPool;
  capacity.racks = {make_id<RackId>("rack-01")};
  CapacityQuantity quantity;
  quantity.unit = "devices";
  quantity.value = 16.0;
  quantity.provenance = EvidenceProvenance::Derived;
  quantity.aggregated = true;
  capacity.quantities = {quantity};
  capacity.header = domain_header(1'000'000, 3, "capacity");
  state.capacity_domains.emplace(capacity.id, capacity);

  FailureDomain failure;
  failure.id = make_id<FailureDomainId>("row-1");
  failure.klass = FailureDomainClass::Row;
  failure.racks = {make_id<RackId>("rack-01"), make_id<RackId>("rack-02")};
  failure.header = domain_header(1'000'000, 3, "failure");
  state.failure_domains.emplace(failure.id, failure);

  NetworkDomain network;
  network.id = make_id<NetworkDomainId>("network-a");
  network.connectivity = ConnectivityClass::SwitchedFabric;
  network.racks = {make_id<RackId>("rack-01")};
  network.header = domain_header(1'000'000, 3, "network");
  state.network_domains.emplace(network.id, network);

  StorageDomain storage;
  storage.id = make_id<StorageDomainId>("storage-a");
  storage.racks = {make_id<RackId>("rack-01")};
  storage.header = domain_header(1'000'000, 3, "storage");
  state.storage_domains.emplace(storage.id, storage);

  PowerDomain power;
  power.id = make_id<PowerDomainId>("power-a");
  power.racks = {make_id<RackId>("rack-01")};
  power.parent = make_id<PowerDomainId>("power-root");
  power.header = domain_header(1'000'000, 3, "power");
  state.power_domains.emplace(power.id, power);

  CoolingDomain cooling;
  cooling.id = make_id<CoolingDomainId>("cooling-a");
  cooling.racks = {make_id<RackId>("rack-01")};
  cooling.parent = make_id<CoolingDomainId>("cooling-root");
  cooling.header = domain_header(1'000'000, 3, "cooling");
  state.cooling_domains.emplace(cooling.id, cooling);

  LinkDomain link_domain;
  link_domain.id = make_id<LinkDomainId>("link-domain-a");
  link_domain.racks = {make_id<RackId>("rack-01")};
  link_domain.header = domain_header(1'000'000, 3, "link_domain");
  state.link_domains.emplace(link_domain.id, link_domain);

  ClusterConstraint constraint;
  constraint.id = make_id<ConstraintId>("constraint-a");
  constraint.kind = ConstraintKind::FailureDomainIndependence;
  constraint.racks = {make_id<RackId>("rack-01"), make_id<RackId>("rack-02")};
  constraint.domain_ref = "failure_domain/row-1";
  constraint.statement = "rack-01 and rack-02 must not share a row";
  constraint.header = domain_header(1'000'000, 3, "constraint");
  state.constraints.emplace(constraint.id, constraint);

  RetiredIdentity retired;
  retired.rack = make_id<RackId>("rack-retired");
  retired.last_generation = RackGeneration::from_raw(2);
  retired.retired_at = Timestamp::from_unix_millis(1'000'000);
  retired.reason = "retired_by_operator";
  state.retired_racks = {retired};

  state.withdrawn_racks = {make_id<RackId>("rack-withdrawn")};

  FencedAuthority fenced;
  fenced.boot = make_id<RackAgentBootId>("boot-fenced");
  fenced.rack = make_id<RackId>("rack-09");
  fenced.reason = "session_closed";
  fenced.fenced_at = Timestamp::from_unix_millis(1'000'000);
  state.fenced_authorities = {fenced};
  return state;
}

/// Advances any strong counter by one, so every generation field can be
/// changed through the same deterministic mutator.
template <class Counter>
void bump(Counter& value) {
  value = Counter::from_raw(value.value() + 1);
}

RackRecord& rack_one(ClusterState& state) {
  const auto it = state.racks.find(make_id<RackId>("rack-01"));
  CF_EXPECT(it != state.racks.end());
  return it->second;
}

InterRackLink& link_one(ClusterState& state) {
  const auto it = state.links.find(make_id<InterRackLinkId>("link-a"));
  CF_EXPECT(it != state.links.end());
  return it->second;
}

std::vector<SnapshotStaleReason> sorted(std::vector<SnapshotStaleReason> reasons) {
  std::sort(reasons.begin(), reasons.end());
  return reasons;
}

std::string reason_names(const std::vector<SnapshotStaleReason>& reasons) {
  std::string out;
  for (const SnapshotStaleReason reason : reasons) {
    if (!out.empty()) {
      out += ",";
    }
    out += std::string(to_string(reason));
  }
  return out;
}

bool contains(const std::vector<SnapshotStaleReason>& reasons, SnapshotStaleReason wanted) {
  return std::find(reasons.begin(), reasons.end(), wanted) != reasons.end();
}

struct StaleCase {
  const char* name;
  SnapshotStaleReason reason;
  std::function<void(ClusterState&)> mutate;
};

std::vector<StaleCase> stale_cases() {
  return {
      {"wrong_cluster", SnapshotStaleReason::WrongCluster,
       [](ClusterState& state) { state.id = make_id<ClusterId>("cluster-b"); }},
      {"cluster_epoch_advanced", SnapshotStaleReason::ClusterEpochAdvanced,
       [](ClusterState& state) { bump(state.epoch); }},
      {"coordinator_epoch_advanced", SnapshotStaleReason::CoordinatorEpochAdvanced,
       [](ClusterState& state) { bump(state.coordinator_epoch); }},
      {"cluster_generation_advanced", SnapshotStaleReason::ClusterGenerationAdvanced,
       [](ClusterState& state) { bump(state.generation); }},
      {"membership_changed", SnapshotStaleReason::MembershipChanged,
       [](ClusterState& state) { bump(state.membership_generation); }},
      {"topology_epoch_superseded", SnapshotStaleReason::TopologyEpochSuperseded,
       [](ClusterState& state) { bump(state.topology_epoch); }},
      {"topology_generation_advanced", SnapshotStaleReason::TopologyGenerationAdvanced,
       [](ClusterState& state) { bump(state.topology_generation); }},
      {"placement_domain_superseded", SnapshotStaleReason::PlacementDomainSuperseded,
       [](ClusterState& state) { bump(state.domain_generations.placement); }},
      {"capacity_domain_superseded", SnapshotStaleReason::CapacityDomainSuperseded,
       [](ClusterState& state) { bump(state.domain_generations.capacity); }},
      {"failure_domain_superseded", SnapshotStaleReason::FailureDomainSuperseded,
       [](ClusterState& state) { bump(state.domain_generations.failure); }},
      {"network_domain_superseded", SnapshotStaleReason::NetworkDomainSuperseded,
       [](ClusterState& state) { bump(state.domain_generations.network); }},
      {"storage_domain_superseded", SnapshotStaleReason::StorageDomainSuperseded,
       [](ClusterState& state) { bump(state.domain_generations.storage); }},
      {"power_domain_superseded", SnapshotStaleReason::PowerDomainSuperseded,
       [](ClusterState& state) { bump(state.domain_generations.power); }},
      {"cooling_domain_superseded", SnapshotStaleReason::CoolingDomainSuperseded,
       [](ClusterState& state) { bump(state.domain_generations.cooling); }},
      {"link_domain_superseded", SnapshotStaleReason::LinkDomainSuperseded,
       [](ClusterState& state) { bump(state.domain_generations.link); }},
      {"connectivity_superseded", SnapshotStaleReason::ConnectivitySuperseded,
       [](ClusterState& state) { bump(state.connectivity_generation); }},
      {"health_superseded", SnapshotStaleReason::HealthSuperseded,
       [](ClusterState& state) { bump(state.health_generation); }},
      {"rack_generation_superseded", SnapshotStaleReason::RackGenerationSuperseded,
       [](ClusterState& state) { bump(rack_one(state).reference.generation); }},
      {"rack_withdrawn", SnapshotStaleReason::RackWithdrawn,
       [](ClusterState& state) {
         rack_one(state).membership = RackMembershipState::Withdrawn;
       }},
      {"rack_retired", SnapshotStaleReason::RackRetired,
       [](ClusterState& state) {
         RetiredIdentity retired;
         retired.rack = make_id<RackId>("rack-01");
         retired.last_generation = RackGeneration::from_raw(7);
         retired.retired_at = Timestamp::from_unix_millis(1'500'000);
         retired.reason = "retired_by_operator";
         state.retired_racks.push_back(retired);
       }},
      {"rack_boot_fenced", SnapshotStaleReason::RackBootFenced,
       [](ClusterState& state) {
         FencedAuthority fenced;
         fenced.boot = make_id<RackAgentBootId>("boot-01");
         fenced.rack = make_id<RackId>("rack-01");
         fenced.reason = "coordinator_restart";
         fenced.fenced_at = Timestamp::from_unix_millis(1'500'000);
         state.fenced_authorities.push_back(fenced);
       }},
      {"rack_revalidation_required", SnapshotStaleReason::RackRevalidationRequired,
       [](ClusterState& state) {
         rack_one(state).reference.currentness = RackCurrentness::RevalidationRequired;
       }},
      {"rack_evidence_revalidation_required", SnapshotStaleReason::RackRevalidationRequired,
       [](ClusterState& state) {
         rack_one(state).reference.evidence.mark_revalidation_required();
       }},
      {"lifecycle_not_consumable", SnapshotStaleReason::LifecycleNotConsumable,
       [](ClusterState& state) { state.lifecycle = ClusterLifecycle::Forming; }},
  };
}

struct DigestCase {
  const char* name;
  std::function<void(ClusterState&)> mutate;
};

std::vector<DigestCase> digest_cases() {
  return {
      {"cluster id", [](ClusterState& s) { s.id = make_id<ClusterId>("cluster-b"); }},
      {"cluster epoch", [](ClusterState& s) { bump(s.epoch); }},
      {"coordinator epoch", [](ClusterState& s) { bump(s.coordinator_epoch); }},
      {"cluster generation", [](ClusterState& s) { bump(s.generation); }},
      {"membership generation", [](ClusterState& s) { bump(s.membership_generation); }},
      {"topology epoch", [](ClusterState& s) { bump(s.topology_epoch); }},
      {"topology generation", [](ClusterState& s) { bump(s.topology_generation); }},
      {"connectivity generation", [](ClusterState& s) { bump(s.connectivity_generation); }},
      {"health generation", [](ClusterState& s) { bump(s.health_generation); }},
      {"constraint generation", [](ClusterState& s) { bump(s.constraint_generation); }},
      {"placement generation", [](ClusterState& s) { bump(s.domain_generations.placement); }},
      {"capacity generation", [](ClusterState& s) { bump(s.domain_generations.capacity); }},
      {"failure generation", [](ClusterState& s) { bump(s.domain_generations.failure); }},
      {"network generation", [](ClusterState& s) { bump(s.domain_generations.network); }},
      {"storage generation", [](ClusterState& s) { bump(s.domain_generations.storage); }},
      {"power generation", [](ClusterState& s) { bump(s.domain_generations.power); }},
      {"cooling generation", [](ClusterState& s) { bump(s.domain_generations.cooling); }},
      {"link generation", [](ClusterState& s) { bump(s.domain_generations.link); }},
      {"snapshot generation", [](ClusterState& s) { bump(s.snapshot_generation); }},
      {"publication generation", [](ClusterState& s) { bump(s.publication_generation); }},
      {"lifecycle", [](ClusterState& s) { s.lifecycle = ClusterLifecycle::Degraded; }},
      {"topology record reason",
       [](ClusterState& s) { s.topology_record.reason = "link_superseded"; }},
      {"topology record established_at",
       [](ClusterState& s) {
         s.topology_record.established_at = Timestamp::from_unix_millis(2'000'000);
       }},
      {"rack identity",
       [](ClusterState& s) {
         RackRecord record = rack_one(s);
         s.racks.erase(make_id<RackId>("rack-01"));
         record.reference.rack = make_id<RackId>("rack-03");
         s.racks.emplace(record.reference.rack, record);
       }},
      {"rack generation", [](ClusterState& s) { bump(rack_one(s).reference.generation); }},
      {"rack membership",
       [](ClusterState& s) { rack_one(s).membership = RackMembershipState::Unavailable; }},
      {"rack lifecycle",
       [](ClusterState& s) {
         rack_one(s).reference.rack_lifecycle = RackLifecycleState::Degraded;
       }},
      {"rack currentness",
       [](ClusterState& s) {
         rack_one(s).reference.currentness = RackCurrentness::Superseded;
       }},
      {"rack health",
       [](ClusterState& s) { rack_one(s).reference.health = HealthState::Degraded; }},
      {"rack authoritative_current",
       [](ClusterState& s) { rack_one(s).authoritative_current = false; }},
      {"rack evidence provenance",
       [](ClusterState& s) {
         rack_one(s).reference.evidence.provenance = EvidenceProvenance::Reported;
       }},
      {"rack evidence freshness",
       [](ClusterState& s) { rack_one(s).reference.evidence.freshness = Freshness::Stale; }},
      {"rack evidence observed_at",
       [](ClusterState& s) {
         rack_one(s).reference.evidence.observed_at = Timestamp::from_unix_millis(1'000'001);
       }},
      {"rack composition label",
       [](ClusterState& s) {
         rack_one(s).reference.composition.composition_label = "cpu-dense";
       }},
      {"rack accelerator vendor",
       [](ClusterState& s) {
         rack_one(s).reference.composition.accelerators[0].vendor = AcceleratorVendor::Amd;
       }},
      {"rack accelerator family",
       [](ClusterState& s) {
         rack_one(s).reference.composition.accelerators[0].family = "H200";
       }},
      {"rack accelerator count",
       [](ClusterState& s) {
         rack_one(s).reference.composition.accelerators[0].device_count = std::uint32_t{16};
       }},
      {"rack accelerator count absent",
       [](ClusterState& s) {
         rack_one(s).reference.composition.accelerators[0].device_count.reset();
       }},
      {"rack endpoint identity",
       [](ClusterState& s) {
         rack_one(s).reference.endpoints[0].id = make_id<RackEndpointId>("endpoint-9");
       }},
      {"rack endpoint connectivity",
       [](ClusterState& s) {
         rack_one(s).reference.endpoints[0].connectivity = ConnectivityClass::ManagementOnly;
       }},
      {"rack failure hint class",
       [](ClusterState& s) {
         rack_one(s).reference.failure_domain_hints[0].klass = FailureDomainClass::NetworkPlane;
       }},
      {"rack failure hint identity",
       [](ClusterState& s) {
         rack_one(s).reference.failure_domain_hints[0].id = make_id<FailureDomainId>("row-2");
       }},
      {"link identity",
       [](ClusterState& s) {
         InterRackLink link = link_one(s);
         s.links.erase(make_id<InterRackLinkId>("link-a"));
         link.id = make_id<InterRackLinkId>("link-b");
         s.links.emplace(link.id, link);
       }},
      {"link source", [](ClusterState& s) { link_one(s).source = make_id<RackId>("rack-02"); }},
      {"link destination",
       [](ClusterState& s) { link_one(s).destination = make_id<RackId>("rack-01"); }},
      {"link direction",
       [](ClusterState& s) { link_one(s).direction = LinkDirection::Unidirectional; }},
      {"link connectivity",
       [](ClusterState& s) { link_one(s).connectivity = ConnectivityClass::RoutedPath; }},
      {"link reachability",
       [](ClusterState& s) { link_one(s).reachability = Reachability::Unreachable; }},
      {"link health", [](ClusterState& s) { link_one(s).health = HealthState::Degraded; }},
      {"link bandwidth class",
       [](ClusterState& s) { link_one(s).bandwidth_class = BandwidthClass::Moderate; }},
      {"link nominal bandwidth",
       [](ClusterState& s) { link_one(s).nominal_bandwidth_bps = std::uint64_t{100}; }},
      {"link nominal bandwidth absent",
       [](ClusterState& s) { link_one(s).nominal_bandwidth_bps.reset(); }},
      {"link latency class",
       [](ClusterState& s) { link_one(s).latency_class = LatencyClass::High; }},
      {"link nominal latency",
       [](ClusterState& s) { link_one(s).nominal_latency_nanos = std::uint64_t{1'000}; }},
      {"link topology epoch", [](ClusterState& s) { bump(link_one(s).topology_epoch); }},
      {"link topology generation",
       [](ClusterState& s) { bump(link_one(s).topology_generation); }},
      {"link header label", [](ClusterState& s) { link_one(s).header.label = "row-b"; }},
      {"link header generation",
       [](ClusterState& s) { bump(link_one(s).header.generation); }},
      {"link failure domains",
       [](ClusterState& s) {
         link_one(s).failure_domains = {make_id<FailureDomainId>("row-2")};
       }},
      {"placement klass",
       [](ClusterState& s) {
         s.placement_domains.begin()->second.klass = PlacementDomainClass::NetworkTier;
       }},
      {"placement members",
       [](ClusterState& s) {
         s.placement_domains.begin()->second.racks = {make_id<RackId>("rack-02")};
       }},
      {"placement header generation",
       [](ClusterState& s) { bump(s.placement_domains.begin()->second.header.generation); }},
      {"capacity klass",
       [](ClusterState& s) {
         s.capacity_domains.begin()->second.klass = CapacityDomainClass::CpuPool;
       }},
      {"capacity quantity value",
       [](ClusterState& s) { s.capacity_domains.begin()->second.quantities[0].value = 32.0; }},
      {"capacity quantity unit",
       [](ClusterState& s) { s.capacity_domains.begin()->second.quantities[0].unit = "cores"; }},
      {"capacity quantity aggregated",
       [](ClusterState& s) { s.capacity_domains.begin()->second.quantities[0].aggregated = false; }},
      {"failure klass",
       [](ClusterState& s) {
         s.failure_domains.begin()->second.klass = FailureDomainClass::PowerFeed;
       }},
      {"failure members",
       [](ClusterState& s) {
         s.failure_domains.begin()->second.racks = {make_id<RackId>("rack-02")};
       }},
      {"network connectivity",
       [](ClusterState& s) {
         s.network_domains.begin()->second.connectivity = ConnectivityClass::Overlay;
       }},
      {"storage members",
       [](ClusterState& s) {
         s.storage_domains.begin()->second.racks = {make_id<RackId>("rack-02")};
       }},
      {"power parent",
       [](ClusterState& s) {
         s.power_domains.begin()->second.parent = make_id<PowerDomainId>("power-other");
       }},
      {"cooling parent",
       [](ClusterState& s) {
         s.cooling_domains.begin()->second.parent = make_id<CoolingDomainId>("cooling-other");
       }},
      {"link domain members",
       [](ClusterState& s) {
         s.link_domains.begin()->second.racks = {make_id<RackId>("rack-02")};
       }},
      {"constraint kind",
       [](ClusterState& s) {
         s.constraints.begin()->second.kind = ConstraintKind::CapacityFloor;
       }},
      {"constraint domain ref",
       [](ClusterState& s) { s.constraints.begin()->second.domain_ref = "power_domain/power-a"; }},
      {"constraint statement",
       [](ClusterState& s) { s.constraints.begin()->second.statement = "changed"; }},
      {"constraint racks",
       [](ClusterState& s) {
         s.constraints.begin()->second.racks = {make_id<RackId>("rack-01")};
       }},
      {"retired rack generation",
       [](ClusterState& s) { bump(s.retired_racks[0].last_generation); }},
      {"retired rack identity",
       [](ClusterState& s) { s.retired_racks[0].rack = make_id<RackId>("rack-retired-2"); }},
      {"withdrawn rack identity",
       [](ClusterState& s) { s.withdrawn_racks[0] = make_id<RackId>("rack-withdrawn-2"); }},
      {"fenced boot identity",
       [](ClusterState& s) { s.fenced_authorities[0].boot = make_id<RackAgentBootId>("boot-x"); }},
      {"fenced rack identity",
       [](ClusterState& s) { s.fenced_authorities[0].rack = make_id<RackId>("rack-10"); }},
      {"fenced reason",
       [](ClusterState& s) { s.fenced_authorities[0].reason = "coordinator_restart"; }},
  };
}

MutationRequest declare_request() {
  MutationRequest request;
  request.kind = MutationKind::DeclareCluster;
  request.cluster = make_id<ClusterId>("cluster-a");
  request.evidence = current_stamp(1'000'000);
  request.requested_at = Timestamp::from_unix_millis(1'000'000);
  request.readiness_contract = ReadinessContract::permissive();
  request.declared_lifecycle = ClusterLifecycle::Ready;
  return request;
}

}  // namespace

// ---------------------------------------------------------------------------
// One concrete scenario per SnapshotStaleReason
// ---------------------------------------------------------------------------

CF_TEST(every_stale_reason_has_a_concrete_isolated_scenario) {
  const ClusterState bound_state = rich_state();
  const ClusterSnapshot snapshot = ClusterSnapshot::capture(bound_state);

  // NONE: a snapshot validated against its own state reports no reason at all.
  const SnapshotValidation current = snapshot.validate(bound_state);
  CF_EXPECT(current.reasons.empty());
  CF_EXPECT(current.subjects.empty());
  CF_EXPECT(current.current);
  CF_EXPECT(current.consumable);
  CF_EXPECT(current.ok());
  CF_EXPECT_EQ(to_string(SnapshotStaleReason::None), std::string_view{"NONE"});
  CF_EXPECT(!contains(current.reasons, SnapshotStaleReason::None));
  CF_EXPECT_EQ(current.describe().substr(0, 16), std::string("snapshot=current"));
  CF_EXPECT_EQ(current.explanation.code, std::string("snapshot_current"));

  const std::vector<StaleCase> cases = stale_cases();
  CF_EXPECT(cases.size() >= 23);
  for (const StaleCase& entry : cases) {
    ClusterState current_state = rich_state();
    entry.mutate(current_state);
    const SnapshotValidation validation = snapshot.validate(current_state);
    if (validation.reasons.size() != 1 || validation.reasons[0] != entry.reason) {
      CF_FAIL(std::string("scenario ") + entry.name + " produced [" +
              reason_names(validation.reasons) + "], expected exactly [" +
              std::string(to_string(entry.reason)) + "]");
    }
    if (validation.subjects.size() != 1 || validation.subjects[0].empty()) {
      CF_FAIL(std::string("scenario ") + entry.name + " produced no subject");
    }
    // `current` is documented as "every bound generation still matches current
    // state". LifecycleNotConsumable is not a generation mismatch, so it is the
    // one reason that leaves `current` true while consumability is false.
    if (validation.consumable) {
      CF_FAIL(std::string("scenario ") + entry.name + " was still reported consumable");
    }
    if (entry.reason != SnapshotStaleReason::LifecycleNotConsumable && validation.current) {
      CF_FAIL(std::string("scenario ") + entry.name + " was still reported current");
    }
    if (validation.describe().find(std::string(to_string(entry.reason))) == std::string::npos) {
      CF_FAIL(std::string("scenario ") + entry.name + " description omitted " +
              std::string(to_string(entry.reason)) + ": " + validation.describe());
    }
    const std::string expected_code =
        validation.current ? std::string("snapshot_current") : std::string("snapshot_stale");
    if (validation.explanation.code != expected_code) {
      CF_FAIL(std::string("scenario ") + entry.name + " explanation code was " +
              validation.explanation.code + ", expected " + expected_code);
    }
    bool factor_seen = false;
    for (const ExplanationFactor& factor : validation.explanation.factors) {
      if (factor.code == std::string(to_string(entry.reason))) {
        factor_seen = true;
      }
    }
    if (!factor_seen) {
      CF_FAIL(std::string("scenario ") + entry.name + " produced no explanation factor for " +
              std::string(to_string(entry.reason)));
    }
  }
}

CF_TEST(stale_reason_is_absent_from_every_other_scenario) {
  const ClusterState bound_state = rich_state();
  const ClusterSnapshot snapshot = ClusterSnapshot::capture(bound_state);
  const std::vector<StaleCase> cases = stale_cases();
  for (const StaleCase& target : cases) {
    ClusterState current_state = rich_state();
    target.mutate(current_state);
    const SnapshotValidation validation = snapshot.validate(current_state);
    for (const StaleCase& other : cases) {
      if (other.reason == target.reason) {
        continue;
      }
      if (contains(validation.reasons, other.reason)) {
        CF_FAIL(std::string("scenario ") + target.name + " also reported " +
                std::string(to_string(other.reason)) + " from " + other.name + ": " +
                reason_names(validation.reasons));
      }
    }
    if (contains(validation.reasons, SnapshotStaleReason::None)) {
      CF_FAIL(std::string("scenario ") + target.name + " reported NONE");
    }
  }
}

CF_TEST(membership_changes_report_membership_reasons) {
  const ClusterSnapshot snapshot = ClusterSnapshot::capture(rich_state());

  // A rack added after the snapshot is a membership change, reported both for
  // the size change and for the new identity.
  ClusterState added = rich_state();
  added.racks.emplace(make_id<RackId>("rack-03"),
                      rack_record("rack-03", "boot-03", 1, RackMembershipState::Active, true));
  const SnapshotValidation added_validation = snapshot.validate(added);
  CF_EXPECT_EQ(added_validation.reasons.size(), std::size_t{2});
  CF_EXPECT_EQ(sorted(added_validation.reasons),
               (std::vector<SnapshotStaleReason>{SnapshotStaleReason::MembershipChanged,
                                                 SnapshotStaleReason::MembershipChanged}));
  bool subject_names_new_rack = false;
  for (const std::string& subject : added_validation.subjects) {
    if (subject.find("rack-03") != std::string::npos) {
      subject_names_new_rack = true;
    }
  }
  CF_EXPECT(subject_names_new_rack);

  // A rack removed after the snapshot is both a membership change and a
  // withdrawal of a bound rack generation.
  ClusterState removed = rich_state();
  removed.racks.erase(make_id<RackId>("rack-01"));
  const SnapshotValidation removed_validation = snapshot.validate(removed);
  CF_EXPECT_EQ(sorted(removed_validation.reasons),
               (std::vector<SnapshotStaleReason>{SnapshotStaleReason::MembershipChanged,
                                                 SnapshotStaleReason::RackWithdrawn}));
}

// ---------------------------------------------------------------------------
// Rack-generation bindings
// ---------------------------------------------------------------------------

CF_TEST(rack_generation_bindings_mirror_canonical_state) {
  const ClusterState state = rich_state();
  const ClusterSnapshot snapshot = ClusterSnapshot::capture(state);
  const std::vector<RackGenerationBinding>& bindings = snapshot.rack_bindings();
  CF_EXPECT_EQ(bindings.size(), state.racks.size());
  CF_EXPECT_EQ(bindings.size(), std::size_t{2});

  std::size_t index = 0;
  for (const auto& entry : state.racks) {
    const RackGenerationBinding& binding = bindings[index];
    CF_EXPECT_EQ(binding.rack, entry.first);
    CF_EXPECT_EQ(binding.generation, entry.second.reference.generation);
    CF_EXPECT_EQ(binding.membership, entry.second.membership);
    CF_EXPECT_EQ(binding.currentness, entry.second.reference.currentness);
    CF_EXPECT_EQ(binding.authoritative_current, entry.second.authoritative_current);
    CF_EXPECT_EQ(binding.boot, entry.second.reference.boot);
    ++index;
  }
  // Bindings are ordered by rack identity.
  CF_EXPECT_EQ(bindings[0].rack, make_id<RackId>("rack-01"));
  CF_EXPECT_EQ(bindings[1].rack, make_id<RackId>("rack-02"));

  const std::optional<RackGeneration> generation = snapshot.rack_generation(make_id<RackId>("rack-01"));
  CF_EXPECT(generation.has_value());
  CF_EXPECT_EQ(*generation, RackGeneration::from_raw(7));
  CF_EXPECT(!snapshot.rack_generation(make_id<RackId>("rack-absent")).has_value());

  // Every binding-driven reason names the affected rack.
  const std::vector<SnapshotStaleReason> binding_reasons = {
      SnapshotStaleReason::RackGenerationSuperseded, SnapshotStaleReason::RackWithdrawn,
      SnapshotStaleReason::RackRetired, SnapshotStaleReason::RackBootFenced,
      SnapshotStaleReason::RackRevalidationRequired};
  for (const StaleCase& entry : stale_cases()) {
    if (std::find(binding_reasons.begin(), binding_reasons.end(), entry.reason) ==
        binding_reasons.end()) {
      continue;
    }
    ClusterState current_state = rich_state();
    entry.mutate(current_state);
    const SnapshotValidation validation = snapshot.validate(current_state);
    CF_EXPECT_EQ(validation.reasons.size(), std::size_t{1});
    if (validation.subjects[0].find("rack/rack-01") == std::string::npos) {
      CF_FAIL(std::string("binding reason ") + std::string(to_string(entry.reason)) +
              " subject was " + validation.subjects[0]);
    }
  }
}

// ---------------------------------------------------------------------------
// Digest stability and coverage
// ---------------------------------------------------------------------------

CF_TEST(semantic_digest_is_stable_and_matches_the_snapshot) {
  const ClusterState state = rich_state();
  const std::string first = semantic_digest_of(state);
  const std::string second = semantic_digest_of(state);
  CF_EXPECT_EQ(first, second);
  CF_EXPECT_EQ(first.size(), std::size_t{16});

  const ClusterSnapshot snapshot = ClusterSnapshot::capture(state);
  CF_EXPECT_EQ(snapshot.semantic_digest(), first);
  const ClusterSnapshot again = ClusterSnapshot::capture(state);
  CF_EXPECT_EQ(again.semantic_digest(), first);
  // Two snapshots of identical state are semantically identical. The snapshot
  // type itself compares its frozen state, not its internal ownership.
  CF_EXPECT(snapshot.state() == again.state());

  // A copy of the state produced by copying the struct is digested identically.
  ClusterState copy = state;
  CF_EXPECT_EQ(semantic_digest_of(copy), first);

  // Process-local metadata that is not part of the semantic content must not
  // perturb the digest: a snapshot is a view of composition, not of authority
  // bookkeeping.
  copy.last_mutation_at = Timestamp::from_unix_millis(9'000'000);
  copy.declared_at = Timestamp::from_unix_millis(9'000'000);
  CF_EXPECT_EQ(semantic_digest_of(copy), first);
}

CF_TEST(semantic_digest_changes_for_every_semantic_field) {
  const ClusterState baseline = rich_state();
  const std::string baseline_digest = semantic_digest_of(baseline);
  const std::vector<DigestCase> cases = digest_cases();
  CF_EXPECT(cases.size() >= 78);
  std::size_t checked = 0;
  for (const DigestCase& entry : cases) {
    ClusterState changed = baseline;
    entry.mutate(changed);
    const std::string digest = semantic_digest_of(changed);
    if (digest == baseline_digest) {
      CF_FAIL(std::string("semantic digest did not change when ") + entry.name + " changed");
    }
    if (digest.size() != 16) {
      CF_FAIL(std::string("digest for ") + entry.name + " was " + digest);
    }
    ++checked;
  }
  CF_EXPECT_EQ(checked, cases.size());

  // Reverting the change restores the digest exactly.
  ClusterState reverted = baseline;
  reverted.health_generation = HealthGeneration::from_raw(4);
  CF_EXPECT_EQ(semantic_digest_of(reverted), baseline_digest);
}

CF_TEST(semantic_digest_ignores_map_insertion_order) {
  ClusterState forward;
  forward.id = make_id<ClusterId>("cluster-a");
  forward.epoch = ClusterEpoch::from_raw(1);
  forward.racks.emplace(make_id<RackId>("rack-01"),
                        rack_record("rack-01", "boot-01", 7, RackMembershipState::Active, true));
  forward.racks.emplace(make_id<RackId>("rack-02"),
                        rack_record("rack-02", "boot-02", 3, RackMembershipState::Active, true));

  ClusterState backward;
  backward.id = make_id<ClusterId>("cluster-a");
  backward.epoch = ClusterEpoch::from_raw(1);
  backward.racks.emplace(make_id<RackId>("rack-02"),
                         rack_record("rack-02", "boot-02", 3, RackMembershipState::Active, true));
  backward.racks.emplace(make_id<RackId>("rack-01"),
                         rack_record("rack-01", "boot-01", 7, RackMembershipState::Active, true));

  CF_EXPECT_EQ(semantic_digest_of(forward), semantic_digest_of(backward));

  // Domain maps behave the same way.
  PlacementDomain one;
  one.id = make_id<PlacementDomainId>("placement-a");
  one.klass = PlacementDomainClass::LowLatencyFabric;
  one.racks = {make_id<RackId>("rack-01")};
  one.header = domain_header(1'000'000, 3, "a");
  PlacementDomain two;
  two.id = make_id<PlacementDomainId>("placement-b");
  two.klass = PlacementDomainClass::NetworkTier;
  two.racks = {make_id<RackId>("rack-02")};
  two.header = domain_header(1'000'000, 3, "b");

  ClusterState ordered;
  ordered.id = forward.id;
  ordered.epoch = forward.epoch;
  ordered.placement_domains.emplace(one.id, one);
  ordered.placement_domains.emplace(two.id, two);

  ClusterState reversed;
  reversed.id = forward.id;
  reversed.epoch = forward.epoch;
  reversed.placement_domains.emplace(two.id, two);
  reversed.placement_domains.emplace(one.id, one);

  CF_EXPECT_EQ(semantic_digest_of(ordered), semantic_digest_of(reversed));
  CF_EXPECT_NE(semantic_digest_of(ordered), semantic_digest_of(forward));
}

// ---------------------------------------------------------------------------
// Lifecycle consumability and revalidation
// ---------------------------------------------------------------------------

CF_TEST(lifecycle_consumability_matches_the_snapshot_validation) {
  const ClusterSnapshot snapshot = ClusterSnapshot::capture(rich_state());
  const ClusterLifecycle not_consumable[] = {ClusterLifecycle::Declared, ClusterLifecycle::Forming,
                                             ClusterLifecycle::RevalidationRequired,
                                             ClusterLifecycle::Retiring, ClusterLifecycle::Retired};
  for (const ClusterLifecycle lifecycle : not_consumable) {
    ClusterState current = rich_state();
    current.lifecycle = lifecycle;
    const SnapshotValidation validation = snapshot.validate(current);
    CF_EXPECT_EQ(validation.reasons.size(), std::size_t{1});
    CF_EXPECT_EQ(validation.reasons[0], SnapshotStaleReason::LifecycleNotConsumable);
    CF_EXPECT_EQ(validation.subjects[0], std::string(to_string(lifecycle)));
    // The generations match, so the snapshot is current; the lifecycle alone
    // makes it non-consumable and adds the typed reason.
    CF_EXPECT(validation.current);
    CF_EXPECT(!validation.consumable);
    CF_EXPECT(!validation.ok());
    CF_EXPECT(validation.requires_revalidation());
    CF_EXPECT(!is_consumable_lifecycle(lifecycle));
  }

  const ClusterLifecycle consumable[] = {ClusterLifecycle::Ready, ClusterLifecycle::Partial,
                                         ClusterLifecycle::Degraded};
  for (const ClusterLifecycle lifecycle : consumable) {
    ClusterState current = rich_state();
    current.lifecycle = lifecycle;
    const SnapshotValidation validation = snapshot.validate(current);
    CF_EXPECT(validation.reasons.empty());
    CF_EXPECT(validation.current);
    CF_EXPECT(validation.consumable);
    CF_EXPECT(validation.ok());
    CF_EXPECT(!validation.requires_revalidation());
    CF_EXPECT(is_consumable_lifecycle(lifecycle));
  }
}

CF_TEST(requires_revalidation_is_reported_for_the_typed_reasons) {
  const SnapshotValidation empty;
  CF_EXPECT(!empty.requires_revalidation());

  const ClusterSnapshot snapshot = ClusterSnapshot::capture(rich_state());
  const SnapshotStaleReason revalidation_reasons[] = {
      SnapshotStaleReason::ClusterEpochAdvanced, SnapshotStaleReason::CoordinatorEpochAdvanced,
      SnapshotStaleReason::RackBootFenced, SnapshotStaleReason::RackRevalidationRequired,
      SnapshotStaleReason::LifecycleNotConsumable};
  for (const StaleCase& entry : stale_cases()) {
    ClusterState current_state = rich_state();
    entry.mutate(current_state);
    const SnapshotValidation validation = snapshot.validate(current_state);
    const bool expected =
        std::find(std::begin(revalidation_reasons), std::end(revalidation_reasons), entry.reason) !=
        std::end(revalidation_reasons);
    if (validation.requires_revalidation() != expected) {
      CF_FAIL(std::string("requires_revalidation for ") + entry.name + " was " +
              (validation.requires_revalidation() ? "true" : "false") + ", expected " +
              (expected ? "true" : "false"));
    }
  }
}

CF_TEST(explanation_factors_are_sorted_and_deduplicated_by_code) {
  const ClusterSnapshot snapshot = ClusterSnapshot::capture(rich_state());
  ClusterState advanced = rich_state();
  bump(advanced.epoch);
  bump(advanced.generation);
  bump(advanced.membership_generation);
  const SnapshotValidation validation = snapshot.validate(advanced);
  CF_EXPECT_EQ(validation.reasons.size(), std::size_t{3});
  CF_EXPECT_EQ(validation.subjects.size(), validation.reasons.size());
  std::string previous;
  for (const ExplanationFactor& factor : validation.explanation.factors) {
    CF_EXPECT(!factor.code.empty());
    if (!previous.empty() && factor.code < previous) {
      CF_FAIL("explanation factors are not sorted by code: " + previous + " then " + factor.code);
    }
    previous = factor.code;
  }
  const std::string described = validation.describe();
  CF_EXPECT(described.find("CLUSTER_EPOCH_ADVANCED") != std::string::npos);
  CF_EXPECT(described.find("CLUSTER_GENERATION_ADVANCED") != std::string::npos);
  CF_EXPECT(described.find("MEMBERSHIP_CHANGED") != std::string::npos);
  CF_EXPECT_EQ(validation.explanation.describe().find("code=snapshot_stale"), std::size_t{0});
  CF_EXPECT_EQ(validation.explanation.factors.size(), validation.reasons.size());
}

// ---------------------------------------------------------------------------
// Validation against a live, advanced coordinator
// ---------------------------------------------------------------------------

CF_TEST(coordinator_snapshot_becomes_stale_after_the_coordinator_advances) {
  ManualClock clock(1'000'000);
  CoordinatorConfig config;
  config.cluster = make_id<ClusterId>("cluster-a");
  config.persist_on_commit = false;
  ClusterCoordinator coordinator(config, nullptr, &clock);
  const CoordinatorStartOutcome started = coordinator.start();
  CF_EXPECT(started.ok);
  CF_EXPECT_EQ(started.status, ProtocolStatus::Ok);

  const ClusterSnapshot before = coordinator.snapshot();
  const std::string before_digest = before.semantic_digest();
  CF_EXPECT_EQ(before.cluster(), make_id<ClusterId>("cluster-a"));
  CF_EXPECT_EQ(before.cluster_generation(), ClusterGeneration::from_raw(0));

  // The coordinator's own snapshot is current against its own authority.
  const SnapshotValidation self = coordinator.validate(before);
  CF_EXPECT(self.current);
  CF_EXPECT_EQ(self.reasons.empty(), is_consumable_lifecycle(before.lifecycle()));

  // Advance the coordinator through its real commit path.
  clock.advance_millis(1'000);
  const MutationResult declared = coordinator.submit(declare_request());
  if (!declared.accepted()) {
    CF_FAIL("DeclareCluster was rejected: " + declared.error.describe());
  }
  const ClusterSnapshot after = coordinator.snapshot();
  CF_EXPECT_NE(after.semantic_digest(), before_digest);
  CF_EXPECT_NE(after.cluster_generation(), before.cluster_generation());
  CF_EXPECT_NE(after.cluster_epoch(), before.cluster_epoch());

  const SnapshotValidation stale = coordinator.validate(before);
  CF_EXPECT(!stale.current);
  CF_EXPECT(!stale.consumable);
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::ClusterEpochAdvanced));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::ClusterGenerationAdvanced));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::MembershipChanged));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::TopologyEpochSuperseded));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::ConnectivitySuperseded));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::HealthSuperseded));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::PlacementDomainSuperseded));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::CapacityDomainSuperseded));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::FailureDomainSuperseded));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::NetworkDomainSuperseded));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::StorageDomainSuperseded));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::PowerDomainSuperseded));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::CoolingDomainSuperseded));
  CF_EXPECT(contains(stale.reasons, SnapshotStaleReason::LinkDomainSuperseded));
  // The coordinator incarnation did not change and the cluster identity still
  // matches, so these two must never be reported.
  CF_EXPECT(!contains(stale.reasons, SnapshotStaleReason::CoordinatorEpochAdvanced));
  CF_EXPECT(!contains(stale.reasons, SnapshotStaleReason::WrongCluster));

  // A snapshot taken after the advance is current again.
  const SnapshotValidation current = coordinator.validate(after);
  CF_EXPECT(current.current);
  CF_EXPECT_EQ(current.reasons.empty(), is_consumable_lifecycle(after.lifecycle()));

  coordinator.stop();
}

CF_TEST(explain_snapshot_staleness_reports_the_typed_reason_codes) {
  ManualClock clock(1'000'000);
  CoordinatorConfig config;
  config.cluster = make_id<ClusterId>("cluster-a");
  config.persist_on_commit = false;
  ClusterCoordinator coordinator(config, nullptr, &clock);
  CF_EXPECT(coordinator.start().ok);

  const ClusterSnapshot snapshot = coordinator.snapshot();
  SnapshotView view = SnapshotView::from_snapshot(snapshot);
  CF_EXPECT_EQ(view.cluster, snapshot.cluster());
  CF_EXPECT_EQ(view.cluster_generation, snapshot.cluster_generation());

  // The explanation for a view must agree with the validation of the same view.
  const SnapshotValidation self = coordinator.validate(snapshot);
  const Explanation current = coordinator.explain_snapshot_staleness(view);
  CF_EXPECT_EQ(current.code, self.reasons.empty() ? std::string("snapshot_current")
                                                  : std::string("snapshot_stale"));
  CF_EXPECT_EQ(current.factors.size(), self.reasons.size());
  if (!self.reasons.empty()) {
    CF_EXPECT(contains(self.reasons, SnapshotStaleReason::LifecycleNotConsumable));
    bool has_lifecycle_factor = false;
    for (const ExplanationFactor& factor : current.factors) {
      if (factor.code == "LIFECYCLE_NOT_CONSUMABLE") {
        has_lifecycle_factor = true;
      }
    }
    CF_EXPECT(has_lifecycle_factor);
  }

  // An advanced view explains with typed reason codes.
  view.cluster_epoch = ClusterEpoch::from_raw(view.cluster_epoch.value() + 1);
  view.cluster_generation = ClusterGeneration::from_raw(view.cluster_generation.value() + 1);
  view.membership_generation = MembershipGeneration::from_raw(view.membership_generation.value() + 1);
  view.domain_generations.placement =
      DomainGeneration::from_raw(view.domain_generations.placement.value() + 1);
  const Explanation stale = coordinator.explain_snapshot_staleness(view);
  CF_EXPECT_EQ(stale.code, std::string("snapshot_stale"));
  CF_EXPECT(!stale.factors.empty());
  const std::string codes[] = {"CLUSTER_EPOCH_ADVANCED", "CLUSTER_GENERATION_ADVANCED",
                               "MEMBERSHIP_CHANGED", "PLACEMENT_DOMAIN_SUPERSEDED"};
  for (const std::string& code : codes) {
    bool found = false;
    for (const ExplanationFactor& factor : stale.factors) {
      if (factor.code == code) {
        found = true;
      }
    }
    if (!found) {
      CF_FAIL("explain_snapshot_staleness omitted " + code + ": " + stale.describe());
    }
  }
  // Factors are sorted by code so the rendering is deterministic.
  std::string previous;
  for (const ExplanationFactor& factor : stale.factors) {
    if (!previous.empty() && factor.code < previous) {
      CF_FAIL("factors are not sorted: " + previous + " then " + factor.code);
    }
    previous = factor.code;
  }

  // The same view validates through the typed API.
  const SnapshotValidation validation = coordinator.validate(view);
  CF_EXPECT(!validation.current);
  CF_EXPECT(contains(validation.reasons, SnapshotStaleReason::ClusterEpochAdvanced));
  CF_EXPECT(contains(validation.reasons, SnapshotStaleReason::ClusterGenerationAdvanced));
  CF_EXPECT(contains(validation.reasons, SnapshotStaleReason::MembershipChanged));
  CF_EXPECT(contains(validation.reasons, SnapshotStaleReason::PlacementDomainSuperseded));

  coordinator.stop();
}

CF_TEST(an_empty_snapshot_is_never_current) {
  const ClusterSnapshot empty;
  CF_EXPECT(!empty.valid());
  const SnapshotValidation validation = empty.validate(rich_state());
  CF_EXPECT(!validation.current);
  CF_EXPECT(!validation.consumable);
  CF_EXPECT(validation.reasons.empty());
  CF_EXPECT_EQ(validation.explanation.code, std::string("snapshot_invalid"));
  CF_EXPECT(empty.semantic_digest().empty());
  CF_EXPECT(empty.rack_bindings().empty());
  CF_EXPECT(!empty.rack_generation(make_id<RackId>("rack-01")).has_value());
}

int main() { return cf_test::run("test_snapshot_staleness"); }
