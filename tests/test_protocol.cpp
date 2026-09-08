// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Framed protocol: exact 28-byte header layout, little-endian framing, CRC32
// coverage, bounded encoders, every truncation prefix, single-bit corruption,
// and adversarial payloads that must be rejected without a large allocation.

#include "harness.hpp"

#include "cluster_fabric/cluster_fabric.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace cluster_fabric;

namespace {

// ---------------------------------------------------------------------------
// Little-endian byte helpers
// ---------------------------------------------------------------------------

void put_u16(std::string& out, std::uint16_t value) {
  out.push_back(static_cast<char>(value & 0xFFu));
  out.push_back(static_cast<char>((value >> 8) & 0xFFu));
}

void put_u32(std::string& out, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    out.push_back(static_cast<char>((value >> (index * 8)) & 0xFFu));
  }
}

void put_u64(std::string& out, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    out.push_back(static_cast<char>((value >> (index * 8)) & 0xFFu));
  }
}

void put_text(std::string& out, std::string_view value) {
  put_u32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value.data(), value.size());
}

std::uint8_t byte_at(std::string_view bytes, std::size_t index) {
  return static_cast<std::uint8_t>(static_cast<unsigned char>(bytes[index]));
}

std::uint16_t read_le16(std::string_view bytes, std::size_t offset) {
  std::uint16_t value = 0;
  for (int index = 0; index < 2; ++index) {
    value = static_cast<std::uint16_t>(
        value | static_cast<std::uint16_t>(byte_at(bytes, offset + static_cast<std::size_t>(index))
                                          << (index * 8)));
  }
  return value;
}

std::uint32_t read_le32(std::string_view bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(byte_at(bytes, offset + static_cast<std::size_t>(index)))
             << (index * 8);
  }
  return value;
}

std::uint64_t read_le64(std::string_view bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(byte_at(bytes, offset + static_cast<std::size_t>(index)))
             << (index * 8);
  }
  return value;
}

// ---------------------------------------------------------------------------
// Fixtures: valid identities and fully populated aggregates
// ---------------------------------------------------------------------------

template <class Id>
Id make_id(std::string_view text) {
  const std::optional<Id> parsed = Id::parse(text);
  CF_EXPECT(parsed.has_value());
  return *parsed;
}

EvidenceStamp stamp(std::int64_t millis, std::int64_t ttl, EvidenceProvenance provenance,
                    Freshness freshness) {
  EvidenceStamp value;
  value.provenance = provenance;
  value.observed_at = Timestamp::from_unix_millis(millis);
  value.ttl_millis = ttl;
  value.freshness = freshness;
  return value;
}

DomainHeader full_domain_header() {
  DomainHeader header;
  header.evidence = stamp(1'700'000'000'000, 60'000, EvidenceProvenance::Measured,
                          Freshness::Fresh);
  header.generation = DomainGeneration::from_raw(9);
  header.publisher = make_id<RackPublisherId>("publisher-a");
  header.cluster_epoch = ClusterEpoch::from_raw(3);
  header.coordinator_epoch = CoordinatorEpoch::from_raw(4);
  header.label = "row-a";
  return header;
}

RackCompositionSummary full_composition() {
  RackCompositionSummary composition;
  AcceleratorClassSummary first;
  first.vendor = AcceleratorVendor::Nvidia;
  first.family = "H100";
  first.device_count = std::uint32_t{8};
  first.device_memory_bytes = std::uint64_t{80ull * 1024ull * 1024ull * 1024ull};
  AcceleratorClassSummary second;
  second.vendor = AcceleratorVendor::Amd;
  second.family = "MI300";
  composition.accelerators = {first, second};
  composition.cpu_sockets = std::uint32_t{2};
  composition.cpu_cores = std::uint32_t{128};
  composition.host_memory_bytes = std::uint64_t{2ull * 1024ull * 1024ull * 1024ull * 1024ull};
  composition.nic_count = std::uint32_t{4};
  composition.switch_count = std::uint32_t{2};
  composition.composition_label = "gpu-dense";
  composition.provenance = EvidenceProvenance::Measured;
  return composition;
}

RackEndpoint full_endpoint() {
  RackEndpoint endpoint;
  endpoint.id = make_id<RackEndpointId>("endpoint-0");
  endpoint.network_domain = make_id<NetworkDomainId>("network-a");
  endpoint.link_domain = make_id<LinkDomainId>("link-domain-a");
  endpoint.connectivity = ConnectivityClass::DirectFabric;
  endpoint.evidence = stamp(1'700'000'000'500, 30'000, EvidenceProvenance::Reported,
                            Freshness::Fresh);
  return endpoint;
}

RackFailureDomainHint full_hint() {
  RackFailureDomainHint hint;
  hint.klass = FailureDomainClass::Row;
  hint.id = make_id<FailureDomainId>("row-1");
  hint.evidence = stamp(1'700'000'000'600, 30'000, EvidenceProvenance::Reported,
                        Freshness::Fresh);
  return hint;
}

RackReference full_rack_reference() {
  RackReference reference;
  reference.rack = make_id<RackId>("rack-01");
  reference.generation = RackGeneration::from_raw(7);
  reference.rack_lifecycle = RackLifecycleState::Ready;
  reference.currentness = RackCurrentness::Current;
  reference.composition = full_composition();
  reference.endpoints = {full_endpoint()};
  reference.failure_domain_hints = {full_hint()};
  reference.evidence = stamp(1'700'000'000'700, 30'000, EvidenceProvenance::Measured,
                             Freshness::Fresh);
  reference.publisher = make_id<RackPublisherId>("publisher-a");
  reference.publication = PublicationGeneration::from_raw(11);
  reference.boot = make_id<RackAgentBootId>("boot-a");
  reference.cluster_epoch = ClusterEpoch::from_raw(3);
  reference.coordinator_epoch = CoordinatorEpoch::from_raw(4);
  reference.health = HealthState::Healthy;
  reference.origin_label = "rack-fabric:1.0.0";
  return reference;
}

MutationAuthority full_authority() {
  MutationAuthority authority;
  authority.cluster_epoch = ClusterEpoch::from_raw(3);
  authority.coordinator_epoch = CoordinatorEpoch::from_raw(4);
  authority.topology_epoch = TopologyEpoch::from_raw(5);
  authority.topology_generation = TopologyGeneration::from_raw(6);
  authority.boot = make_id<RackAgentBootId>("boot-a");
  authority.publisher = make_id<RackPublisherId>("publisher-a");
  authority.rack = make_id<RackId>("rack-01");
  authority.rack_generation = RackGeneration::from_raw(7);
  authority.publication = PublicationGeneration::from_raw(11);
  return authority;
}

ReadinessContract full_contract() {
  ReadinessContract contract;
  contract.minimum_current_racks = 2;
  contract.mandatory_racks = {make_id<RackId>("rack-01"), make_id<RackId>("rack-02")};
  contract.require_all_active_racks_current = true;
  contract.require_connectivity_evidence = true;
  contract.minimum_current_links = 1;
  contract.required_placement_domain_classes = {PlacementDomainClass::AvailabilityDomain};
  contract.required_failure_domain_classes = {FailureDomainClass::Row};
  contract.allow_partial = false;
  contract.allow_degraded = false;
  contract.require_no_conflicts = true;
  return contract;
}

PlacementDomain full_placement_domain() {
  PlacementDomain domain;
  domain.id = make_id<PlacementDomainId>("placement-a");
  domain.klass = PlacementDomainClass::LowLatencyFabric;
  domain.racks = {make_id<RackId>("rack-01"), make_id<RackId>("rack-02")};
  domain.header = full_domain_header();
  return domain;
}

CapacityDomain full_capacity_domain() {
  CapacityDomain domain;
  domain.id = make_id<CapacityDomainId>("capacity-a");
  domain.klass = CapacityDomainClass::AcceleratorPool;
  domain.racks = {make_id<RackId>("rack-01")};
  CapacityQuantity quantity;
  quantity.unit = "devices";
  quantity.value = 16.5;
  quantity.provenance = EvidenceProvenance::Derived;
  quantity.aggregated = true;
  domain.quantities = {quantity};
  domain.header = full_domain_header();
  return domain;
}

FailureDomain full_failure_domain() {
  FailureDomain domain;
  domain.id = make_id<FailureDomainId>("row-1");
  domain.klass = FailureDomainClass::Row;
  domain.racks = {make_id<RackId>("rack-01")};
  domain.header = full_domain_header();
  return domain;
}

NetworkDomain full_network_domain() {
  NetworkDomain domain;
  domain.id = make_id<NetworkDomainId>("network-a");
  domain.connectivity = ConnectivityClass::SwitchedFabric;
  domain.racks = {make_id<RackId>("rack-01")};
  domain.header = full_domain_header();
  return domain;
}

StorageDomain full_storage_domain() {
  StorageDomain domain;
  domain.id = make_id<StorageDomainId>("storage-a");
  domain.racks = {make_id<RackId>("rack-01")};
  domain.header = full_domain_header();
  return domain;
}

PowerDomain full_power_domain() {
  PowerDomain domain;
  domain.id = make_id<PowerDomainId>("power-a");
  domain.racks = {make_id<RackId>("rack-01")};
  domain.parent = make_id<PowerDomainId>("power-root");
  domain.header = full_domain_header();
  return domain;
}

CoolingDomain full_cooling_domain() {
  CoolingDomain domain;
  domain.id = make_id<CoolingDomainId>("cooling-a");
  domain.racks = {make_id<RackId>("rack-01")};
  domain.parent = make_id<CoolingDomainId>("cooling-root");
  domain.header = full_domain_header();
  return domain;
}

LinkDomain full_link_domain() {
  LinkDomain domain;
  domain.id = make_id<LinkDomainId>("link-domain-a");
  domain.racks = {make_id<RackId>("rack-01")};
  domain.header = full_domain_header();
  return domain;
}

ClusterConstraint full_constraint() {
  ClusterConstraint constraint;
  constraint.id = make_id<ConstraintId>("constraint-a");
  constraint.kind = ConstraintKind::FailureDomainIndependence;
  constraint.racks = {make_id<RackId>("rack-01"), make_id<RackId>("rack-02")};
  constraint.domain_ref = "failure_domain/row-1";
  constraint.statement = "rack-01 and rack-02 must not share a row";
  constraint.header = full_domain_header();
  return constraint;
}

InterRackLink full_link() {
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
  link.header = full_domain_header();
  link.topology_epoch = TopologyEpoch::from_raw(5);
  link.topology_generation = TopologyGeneration::from_raw(6);
  link.publication = PublicationGeneration::from_raw(11);
  link.boot = make_id<RackAgentBootId>("boot-a");
  return link;
}

StructuredError full_error() {
  StructuredError error;
  error.category = ErrorCategory::Protocol;
  error.stage = ErrorStage::Decode;
  error.subject = "frame";
  error.reason = "checksum_mismatch";
  error.expected = "checksum=0x12345678";
  error.current = "checksum=0x00000000";
  error.message = "frame checksum does not match the header and payload";
  return error;
}

Explanation full_explanation() {
  Explanation explanation = Explanation::make("snapshot_stale", "snapshot", "one reason");
  explanation.add("MEMBERSHIP_CHANGED", "rack/rack-01", "member added after the snapshot");
  explanation.add("WRONG_CLUSTER", "snapshot/other", "the snapshot targets another cluster");
  explanation.sort_factors();
  return explanation;
}

SnapshotValidation full_validation() {
  SnapshotValidation validation;
  validation.current = false;
  validation.consumable = false;
  validation.reasons = {SnapshotStaleReason::MembershipChanged, SnapshotStaleReason::WrongCluster};
  validation.subjects = {"rack/rack-01", "snapshot/other"};
  validation.explanation = full_explanation();
  return validation;
}

ReadinessEvaluation full_readiness() {
  ReadinessEvaluation evaluation;
  evaluation.satisfied = false;
  evaluation.lifecycle = ClusterLifecycle::Forming;
  ReadinessBlocker blocker;
  blocker.code = "mandatory_rack_missing";
  blocker.subject = "rack/rack-01";
  blocker.detail = "rack is mandatory and has no canonical record";
  evaluation.blockers = {blocker};
  return evaluation;
}

MutationRequest full_mutation_request() {
  MutationRequest request;
  request.kind = MutationKind::PublishInterRackLink;
  request.cluster = make_id<ClusterId>("cluster-a");
  request.authority = full_authority();
  request.evidence = stamp(1'700'000'000'800, 30'000, EvidenceProvenance::Measured,
                           Freshness::Fresh);
  request.reason = "operator note";
  request.requested_at = Timestamp::from_unix_millis(1'700'000'000'900);
  request.declared_lifecycle = ClusterLifecycle::Partial;
  request.readiness_contract = full_contract();
  request.rack_reference = full_rack_reference();
  request.membership = RackMembershipState::Active;
  request.expected_rack_generation = RackGeneration::from_raw(6);
  request.new_rack_generation = RackGeneration::from_raw(7);
  request.placement_domain = full_placement_domain();
  request.capacity_domain = full_capacity_domain();
  request.failure_domain = full_failure_domain();
  request.network_domain = full_network_domain();
  request.storage_domain = full_storage_domain();
  request.power_domain = full_power_domain();
  request.cooling_domain = full_cooling_domain();
  request.link_domain = full_link_domain();
  request.constraint = full_constraint();
  request.link = full_link();
  request.link_id = make_id<InterRackLinkId>("link-a");
  request.target_topology_epoch = TopologyEpoch::from_raw(6);
  request.topology_reason = "link_superseded";
  request.health = HealthState::Degraded;
  request.reachability = Reachability::Reachable;
  request.evidence_selector = "rack_health";
  request.racks = {make_id<RackId>("rack-01"), make_id<RackId>("rack-02")};
  return request;
}

MutationRequest minimal_mutation_request() {
  MutationRequest request;
  request.kind = MutationKind::Unknown;
  request.cluster = make_id<ClusterId>("cluster-a");
  request.rack_reference.rack = make_id<RackId>("rack-01");
  request.rack_reference.boot = make_id<RackAgentBootId>("boot-a");
  return request;
}

MutationResult full_mutation_result() {
  MutationResult result;
  result.outcome = MutationOutcome::Rejected;
  result.reason = RejectionReason::StaleRackGeneration;
  result.error = full_error();
  result.explanation = full_explanation();
  result.cluster_epoch = ClusterEpoch::from_raw(3);
  result.coordinator_epoch = CoordinatorEpoch::from_raw(4);
  result.cluster_generation = ClusterGeneration::from_raw(5);
  result.membership_generation = MembershipGeneration::from_raw(6);
  result.topology_epoch = TopologyEpoch::from_raw(7);
  result.topology_generation = TopologyGeneration::from_raw(8);
  result.connectivity_generation = ConnectivityGeneration::from_raw(9);
  result.health_generation = HealthGeneration::from_raw(10);
  result.constraint_generation = ConstraintGeneration::from_raw(11);
  result.snapshot_generation = SnapshotGeneration::from_raw(12);
  result.publication_generation = PublicationGeneration::from_raw(13);
  result.lifecycle = ClusterLifecycle::Degraded;
  return result;
}

SnapshotView full_snapshot_view() {
  SnapshotView view;
  view.cluster = make_id<ClusterId>("cluster-a");
  view.cluster_epoch = ClusterEpoch::from_raw(3);
  view.coordinator_epoch = CoordinatorEpoch::from_raw(4);
  view.cluster_generation = ClusterGeneration::from_raw(5);
  view.membership_generation = MembershipGeneration::from_raw(6);
  view.topology_epoch = TopologyEpoch::from_raw(7);
  view.topology_generation = TopologyGeneration::from_raw(8);
  view.connectivity_generation = ConnectivityGeneration::from_raw(9);
  view.health_generation = HealthGeneration::from_raw(10);
  view.constraint_generation = ConstraintGeneration::from_raw(11);
  view.domain_generations.placement = DomainGeneration::from_raw(12);
  view.domain_generations.capacity = DomainGeneration::from_raw(13);
  view.domain_generations.failure = DomainGeneration::from_raw(14);
  view.domain_generations.network = DomainGeneration::from_raw(15);
  view.domain_generations.storage = DomainGeneration::from_raw(16);
  view.domain_generations.power = DomainGeneration::from_raw(17);
  view.domain_generations.cooling = DomainGeneration::from_raw(18);
  view.domain_generations.link = DomainGeneration::from_raw(19);
  view.snapshot_generation = SnapshotGeneration::from_raw(20);
  view.publication_generation = PublicationGeneration::from_raw(21);
  view.lifecycle = ClusterLifecycle::Ready;
  RackGenerationBinding binding;
  binding.rack = make_id<RackId>("rack-01");
  binding.generation = RackGeneration::from_raw(7);
  binding.membership = RackMembershipState::Active;
  binding.currentness = RackCurrentness::Current;
  binding.authoritative_current = true;
  binding.boot = make_id<RackAgentBootId>("boot-a");
  view.rack_bindings = {binding};
  view.semantic_digest = "0123456789abcdef";
  view.rack_count = 1;
  view.link_count = 2;
  view.domain_count = 3;
  return view;
}

// ---------------------------------------------------------------------------
// Fully populated and minimal message fixtures
// ---------------------------------------------------------------------------

HelloMessage full_hello() {
  HelloMessage message;
  message.protocol_version = kProtocolVersion;
  message.cluster = make_id<ClusterId>("cluster-a");
  message.boot = make_id<RackAgentBootId>("boot-a");
  message.role = "publisher";
  message.build = version_banner();
  return message;
}

HelloMessage minimal_hello() {
  HelloMessage message;
  message.protocol_version = kProtocolVersion;
  message.cluster = make_id<ClusterId>("cluster-a");
  message.boot = make_id<RackAgentBootId>("boot-a");
  return message;
}

HelloAckMessage full_hello_ack() {
  HelloAckMessage message;
  message.protocol_version = kProtocolVersion;
  message.cluster = make_id<ClusterId>("cluster-a");
  message.coordinator_epoch = CoordinatorEpoch::from_raw(4);
  message.cluster_epoch = ClusterEpoch::from_raw(3);
  message.topology_epoch = TopologyEpoch::from_raw(5);
  message.topology_generation = TopologyGeneration::from_raw(6);
  message.lifecycle = ClusterLifecycle::Ready;
  message.accepted = true;
  message.reason = RejectionReason::None;
  message.detail = "accepted";
  return message;
}

HelloAckMessage minimal_hello_ack() {
  HelloAckMessage message;
  message.protocol_version = kProtocolVersion;
  message.cluster = make_id<ClusterId>("cluster-a");
  message.accepted = false;
  return message;
}

RegisterPublisherMessage full_register_publisher() {
  RegisterPublisherMessage message;
  message.cluster = make_id<ClusterId>("cluster-a");
  message.authority = full_authority();
  message.publisher = make_id<RackPublisherId>("publisher-a");
  message.rack = make_id<RackId>("rack-01");
  message.generation = RackGeneration::from_raw(7);
  message.rack_lifecycle = RackLifecycleState::Ready;
  message.composition = full_composition();
  message.origin_label = "rack-fabric:1.0.0";
  message.sent_at = Timestamp::from_unix_millis(1'700'000'000'900);
  return message;
}

RegisterPublisherMessage minimal_register_publisher() {
  RegisterPublisherMessage message;
  message.cluster = make_id<ClusterId>("cluster-a");
  message.publisher = make_id<RackPublisherId>("publisher-a");
  message.rack = make_id<RackId>("rack-01");
  message.generation = RackGeneration::from_raw(1);
  return message;
}

RegisterAckMessage full_register_ack() {
  RegisterAckMessage message;
  message.accepted = true;
  message.reason = RejectionReason::None;
  message.coordinator_epoch = CoordinatorEpoch::from_raw(4);
  message.cluster_epoch = ClusterEpoch::from_raw(3);
  message.topology_epoch = TopologyEpoch::from_raw(5);
  message.topology_generation = TopologyGeneration::from_raw(6);
  message.membership_generation = MembershipGeneration::from_raw(6);
  message.detail = "registered";
  return message;
}

RegisterAckMessage minimal_register_ack() {
  RegisterAckMessage message;
  message.accepted = false;
  message.reason = RejectionReason::Malformed;
  return message;
}

PublishRequestMessage full_publish_request() {
  PublishRequestMessage message;
  message.request = full_mutation_request();
  return message;
}

PublishRequestMessage minimal_publish_request() {
  PublishRequestMessage message;
  message.request = minimal_mutation_request();
  return message;
}

PublishResultMessage full_publish_result() {
  PublishResultMessage message;
  message.result = full_mutation_result();
  return message;
}

PublishResultMessage minimal_publish_result() {
  PublishResultMessage message;
  message.result = MutationResult{};
  return message;
}

HeartbeatMessage full_heartbeat() {
  HeartbeatMessage message;
  message.cluster = make_id<ClusterId>("cluster-a");
  message.authority = full_authority();
  message.sent_at = Timestamp::from_unix_millis(1'700'000'000'900);
  return message;
}

HeartbeatMessage minimal_heartbeat() {
  HeartbeatMessage message;
  message.cluster = make_id<ClusterId>("cluster-a");
  return message;
}

HeartbeatAckMessage full_heartbeat_ack() {
  HeartbeatAckMessage message;
  message.coordinator_epoch = CoordinatorEpoch::from_raw(4);
  message.cluster_epoch = ClusterEpoch::from_raw(3);
  message.topology_epoch = TopologyEpoch::from_raw(5);
  message.lifecycle = ClusterLifecycle::Ready;
  message.accepted = true;
  message.reason = RejectionReason::None;
  message.detail = "current";
  return message;
}

HeartbeatAckMessage minimal_heartbeat_ack() {
  HeartbeatAckMessage message;
  message.accepted = false;
  message.reason = RejectionReason::StaleClusterEpoch;
  return message;
}

SnapshotRequestMessage full_snapshot_request() {
  SnapshotRequestMessage message;
  message.cluster = make_id<ClusterId>("cluster-a");
  return message;
}

SnapshotResponseMessage full_snapshot_response() {
  SnapshotResponseMessage message;
  message.accepted = true;
  message.reason = RejectionReason::None;
  message.detail = "snapshot attached";
  message.view = full_snapshot_view();
  return message;
}

SnapshotResponseMessage minimal_snapshot_response() {
  SnapshotResponseMessage message;
  message.accepted = false;
  message.reason = RejectionReason::WrongCluster;
  message.view.cluster = make_id<ClusterId>("cluster-a");
  return message;
}

ValidateSnapshotRequestMessage full_validate_request() {
  ValidateSnapshotRequestMessage message;
  message.view = full_snapshot_view();
  return message;
}

ValidateSnapshotRequestMessage minimal_validate_request() {
  ValidateSnapshotRequestMessage message;
  message.view.cluster = make_id<ClusterId>("cluster-a");
  return message;
}

ValidateSnapshotResponseMessage full_validate_response() {
  ValidateSnapshotResponseMessage message;
  message.validation = full_validation();
  return message;
}

ValidateSnapshotResponseMessage minimal_validate_response() {
  ValidateSnapshotResponseMessage message;
  return message;
}

RevalidateRequestMessage full_revalidate_request() {
  RevalidateRequestMessage message;
  message.cluster = make_id<ClusterId>("cluster-a");
  message.authority = full_authority();
  message.racks = {make_id<RackId>("rack-01"), make_id<RackId>("rack-02")};
  return message;
}

RevalidateRequestMessage minimal_revalidate_request() {
  RevalidateRequestMessage message;
  message.cluster = make_id<ClusterId>("cluster-a");
  return message;
}

RevalidateResponseMessage full_revalidate_response() {
  RevalidateResponseMessage message;
  message.accepted = true;
  message.reason = RejectionReason::None;
  message.detail = "revalidated";
  message.lifecycle = ClusterLifecycle::Ready;
  message.readiness = full_readiness();
  return message;
}

RevalidateResponseMessage minimal_revalidate_response() {
  RevalidateResponseMessage message;
  message.accepted = false;
  message.reason = RejectionReason::NotReady;
  return message;
}

ShutdownMessage full_shutdown() {
  ShutdownMessage message;
  message.reason = "operator requested shutdown";
  return message;
}

ShutdownMessage minimal_shutdown() { return ShutdownMessage{}; }

ErrorMessage full_error_message() {
  ErrorMessage message;
  message.error = full_error();
  return message;
}

ErrorMessage minimal_error_message() { return ErrorMessage{}; }

// ---------------------------------------------------------------------------
// Generic round-trip driver
// ---------------------------------------------------------------------------

/// Encodes, decodes and re-encodes p message and asserts the payload is
/// byte-identical, that decoding consumed the whole payload, and that the
/// frame carrying it round-trips. Equality is asserted when the message type
/// provides operator==.
template <class Message>
void expect_message_round_trip(const Message& message, std::string (*encode)(const Message&),
                               DecodeOutcome (*decode)(std::string_view, Message&),
                               MessageType type, const char* label) {
  const std::string payload = encode(message);
  if (payload.empty()) {
    CF_FAIL(std::string("encoder produced an empty payload for ") + label);
  }
  Message decoded{};
  const DecodeOutcome outcome = decode(payload, decoded);
  if (outcome.status != ProtocolStatus::Ok) {
    CF_FAIL(std::string("decode of ") + label + " failed with " +
            std::string(to_string(outcome.status)) + " (" + outcome.error.message + ")");
  }
  const std::string reencoded = encode(decoded);
  if (reencoded != payload) {
    CF_FAIL(std::string("re-encode of ") + label + " is not byte-identical (" +
            std::to_string(payload.size()) + " vs " + std::to_string(reencoded.size()) + " bytes)");
  }
  if constexpr (requires(const Message& lhs, const Message& rhs) { lhs == rhs; }) {
    if (!(decoded == message)) {
      CF_FAIL(std::string("decoded ") + label + " is not equal to the original");
    }
  }

  const std::string frame = encode_frame(type, 42, payload, 0x5A5A5A5Au);
  if (frame.empty()) {
    CF_FAIL(std::string("frame encoding failed for ") + label);
  }
  const FrameOutcome framed = decode_frame(frame);
  if (framed.status != ProtocolStatus::Ok) {
    CF_FAIL(std::string("frame decode of ") + label + " failed with " +
            std::string(to_string(framed.status)));
  }
  if (framed.header.type != type) {
    CF_FAIL(std::string("frame type mismatch for ") + label);
  }
  if (framed.payload != payload) {
    CF_FAIL(std::string("frame payload mismatch for ") + label);
  }
  if (framed.consumed != frame.size()) {
    CF_FAIL(std::string("frame consumed mismatch for ") + label);
  }
  Message from_frame{};
  if (decode(framed.payload, from_frame).status != ProtocolStatus::Ok) {
    CF_FAIL(std::string("payload from frame did not decode for ") + label);
  }
}

/// Same driver for the shared codecs that take a ByteWriter/ByteReader.
template <class Value, class EncodeFn, class DecodeFn>
void expect_codec_round_trip(const Value& value, EncodeFn&& encode, DecodeFn&& decode,
                             const char* label) {
  ByteWriter writer;
  if (!encode(value, writer)) {
    CF_FAIL(std::string("shared encoder rejected ") + label);
  }
  if (!writer.ok()) {
    CF_FAIL(std::string("shared encoder reported ") + std::string(to_string(writer.status())) +
            " for " + label);
  }
  const std::string bytes = writer.data();
  ByteReader reader(bytes);
  Value decoded{};
  const DecodeOutcome outcome = decode(reader, decoded);
  if (outcome.status != ProtocolStatus::Ok) {
    CF_FAIL(std::string("shared decoder rejected ") + label + " with " +
            std::string(to_string(outcome.status)) + " (" + outcome.error.message + ")");
  }
  if (!reader.exhausted()) {
    CF_FAIL(std::string("shared decoder left ") + std::to_string(reader.remaining()) +
            " byte(s) for " + label);
  }
  ByteWriter again;
  if (!encode(decoded, again)) {
    CF_FAIL(std::string("re-encode rejected ") + label);
  }
  if (again.data() != bytes) {
    CF_FAIL(std::string("shared codec re-encode of ") + label + " is not byte-identical");
  }
}

/// StructuredError has no operator==, so equality is asserted field by field.
void expect_same_error(const StructuredError& actual, const StructuredError& expected,
                       const char* label) {
  if (actual.category != expected.category || actual.stage != expected.stage ||
      actual.subject != expected.subject || actual.reason != expected.reason ||
      actual.expected != expected.expected || actual.current != expected.current ||
      actual.message != expected.message) {
    CF_FAIL(std::string("structured error mismatch for ") + label + ": reason=" + actual.reason +
            " expected=" + actual.expected + " current=" + actual.current);
  }
}

/// The status decode_frame must report for one single-bit corruption, derived
/// from the documented check order (magic, version, message type, declared
/// length bound, declared length availability, checksum).
ProtocolStatus expected_status_for_corruption(std::string_view frame, std::uint32_t max_payload) {
  const std::uint32_t bounded_max = max_payload > kAbsoluteMaxFramePayloadBytes
                                        ? kAbsoluteMaxFramePayloadBytes
                                        : max_payload;
  if (read_le32(frame, 0) != kProtocolMagic) {
    return ProtocolStatus::BadMagic;
  }
  if (read_le16(frame, 4) != kProtocolVersion) {
    return ProtocolStatus::UnsupportedVersion;
  }
  if (static_cast<std::uint8_t>(read_le16(frame, 6)) > 16u) {
    return ProtocolStatus::UnknownMessage;
  }
  const std::uint32_t declared = read_le32(frame, 20);
  if (declared > bounded_max) {
    return ProtocolStatus::OversizedFrame;
  }
  if (static_cast<std::size_t>(declared) > frame.size() - kFrameHeaderSize) {
    return ProtocolStatus::TruncatedPayload;
  }
  return ProtocolStatus::ChecksumMismatch;
}

std::string flip_bit(std::string frame, std::size_t index, unsigned bit) {
  frame[index] = static_cast<char>(static_cast<unsigned char>(frame[index]) ^
                                   static_cast<unsigned char>(1u << bit));
  return frame;
}

}  // namespace

// ---------------------------------------------------------------------------
// Header layout and framing
// ---------------------------------------------------------------------------

CF_TEST(crc32_matches_the_ieee_reference) {
  CF_EXPECT_EQ(crc32(""), 0u);
  CF_EXPECT_EQ(crc32("123456789"), 0xCBF43926u);
  CF_EXPECT_EQ(crc32("a"), 0xE8B7BE43u);
}

CF_TEST(frame_header_layout_is_exact_and_little_endian) {
  const std::string payload = "payload-bytes";
  const std::string frame =
      encode_frame(MessageType::SnapshotResponse, 0x1122334455667788ull, payload, 0xAABBCCDDu);
  CF_EXPECT_EQ(frame.size(), kFrameHeaderSize + payload.size());

  static_assert(kFrameHeaderSize == 28, "the frame header is exactly 28 bytes");
  static_assert(kProtocolMagic == 0x31465043u, "the frame magic is 'CPF1' little-endian");
  CF_EXPECT_EQ(byte_at(frame, 0), std::uint8_t{0x43});
  CF_EXPECT_EQ(byte_at(frame, 1), std::uint8_t{0x50});
  CF_EXPECT_EQ(byte_at(frame, 2), std::uint8_t{0x46});
  CF_EXPECT_EQ(byte_at(frame, 3), std::uint8_t{0x31});
  CF_EXPECT_EQ(read_le32(frame, 0), kProtocolMagic);

  CF_EXPECT_EQ(byte_at(frame, 4), std::uint8_t{0x01});
  CF_EXPECT_EQ(byte_at(frame, 5), std::uint8_t{0x00});
  CF_EXPECT_EQ(read_le16(frame, 4), kProtocolVersion);

  CF_EXPECT_EQ(byte_at(frame, 6), std::uint8_t{0x0A});
  CF_EXPECT_EQ(byte_at(frame, 7), std::uint8_t{0x00});
  CF_EXPECT_EQ(read_le16(frame, 6), static_cast<std::uint16_t>(MessageType::SnapshotResponse));

  CF_EXPECT_EQ(byte_at(frame, 8), std::uint8_t{0xDD});
  CF_EXPECT_EQ(byte_at(frame, 9), std::uint8_t{0xCC});
  CF_EXPECT_EQ(byte_at(frame, 10), std::uint8_t{0xBB});
  CF_EXPECT_EQ(byte_at(frame, 11), std::uint8_t{0xAA});
  CF_EXPECT_EQ(read_le32(frame, 8), 0xAABBCCDDu);

  CF_EXPECT_EQ(byte_at(frame, 12), std::uint8_t{0x88});
  CF_EXPECT_EQ(byte_at(frame, 19), std::uint8_t{0x11});
  CF_EXPECT_EQ(read_le64(frame, 12), 0x1122334455667788ull);

  CF_EXPECT_EQ(read_le32(frame, 20), static_cast<std::uint32_t>(payload.size()));
  CF_EXPECT_EQ(byte_at(frame, 20), std::uint8_t{0x0D});
  CF_EXPECT_EQ(byte_at(frame, 21), std::uint8_t{0x00});
  CF_EXPECT_EQ(byte_at(frame, 22), std::uint8_t{0x00});
  CF_EXPECT_EQ(byte_at(frame, 23), std::uint8_t{0x00});

  const std::string covered = std::string(frame.substr(0, 24)) + payload;
  const std::uint32_t expected_checksum = crc32(covered);
  CF_EXPECT_EQ(read_le32(frame, 24), expected_checksum);
  CF_EXPECT_NE(expected_checksum, crc32(payload));
  CF_EXPECT_NE(expected_checksum, crc32(frame.substr(0, 24)));
  CF_EXPECT_EQ(frame.substr(kFrameHeaderSize), payload);

  const FrameOutcome decoded = decode_frame(frame);
  CF_EXPECT_EQ(decoded.status, ProtocolStatus::Ok);
  CF_EXPECT_EQ(decoded.header.magic, kProtocolMagic);
  CF_EXPECT_EQ(decoded.header.version, kProtocolVersion);
  CF_EXPECT_EQ(decoded.header.type, MessageType::SnapshotResponse);
  CF_EXPECT_EQ(decoded.header.flags, 0xAABBCCDDu);
  CF_EXPECT_EQ(decoded.header.sequence, 0x1122334455667788ull);
  CF_EXPECT_EQ(decoded.header.payload_length, static_cast<std::uint32_t>(payload.size()));
  CF_EXPECT_EQ(decoded.header.checksum, expected_checksum);
  CF_EXPECT_EQ(decoded.payload, payload);
  CF_EXPECT_EQ(decoded.consumed, frame.size());
}

CF_TEST(frame_round_trip_ignores_trailing_bytes_and_packs_two_frames) {
  const std::string first_payload = encode_hello(full_hello());
  const std::string second_payload = encode_shutdown(full_shutdown());
  CF_EXPECT(!first_payload.empty());
  CF_EXPECT(!second_payload.empty());

  const std::string first = encode_frame(MessageType::Hello, 1, first_payload);
  const std::string second = encode_frame(MessageType::Shutdown, 2, second_payload, 0x7u);
  CF_EXPECT(!first.empty());
  CF_EXPECT(!second.empty());

  const std::string trailing = first + "trailing-garbage-after-a-complete-frame";
  const FrameOutcome only_first = decode_frame(trailing);
  CF_EXPECT_EQ(only_first.status, ProtocolStatus::Ok);
  CF_EXPECT_EQ(only_first.consumed, first.size());
  CF_EXPECT_EQ(only_first.header.type, MessageType::Hello);
  CF_EXPECT_EQ(only_first.header.sequence, 1ull);
  CF_EXPECT_EQ(only_first.header.flags, 0u);
  CF_EXPECT_EQ(only_first.payload, first_payload);

  const std::string packed = first + second;
  const FrameOutcome frame_one = decode_frame(packed);
  CF_EXPECT_EQ(frame_one.status, ProtocolStatus::Ok);
  CF_EXPECT_EQ(frame_one.consumed, first.size());
  const FrameOutcome frame_two = decode_frame(std::string_view(packed).substr(frame_one.consumed));
  CF_EXPECT_EQ(frame_two.status, ProtocolStatus::Ok);
  CF_EXPECT_EQ(frame_two.consumed, second.size());
  CF_EXPECT_EQ(frame_two.header.type, MessageType::Shutdown);
  CF_EXPECT_EQ(frame_two.header.sequence, 2ull);
  CF_EXPECT_EQ(frame_two.header.flags, 0x7u);
  CF_EXPECT_EQ(frame_two.payload, second_payload);
  CF_EXPECT_EQ(frame_one.consumed + frame_two.consumed, packed.size());

  // An empty payload is a legal frame and consumes exactly the header.
  const std::string empty_frame = encode_frame(MessageType::HeartbeatAck, 3, "");
  CF_EXPECT_EQ(empty_frame.size(), kFrameHeaderSize);
  const FrameOutcome empty_decoded = decode_frame(empty_frame);
  CF_EXPECT_EQ(empty_decoded.status, ProtocolStatus::Ok);
  CF_EXPECT_EQ(empty_decoded.header.payload_length, 0u);
  CF_EXPECT_EQ(empty_decoded.consumed, kFrameHeaderSize);
  CF_EXPECT(empty_decoded.payload.empty());
}

CF_TEST(frame_truncation_at_every_prefix_length_is_rejected) {
  const std::string payload = "0123456789abcdef";
  const std::string frame = encode_frame(MessageType::PublishRequest, 9, payload);
  CF_EXPECT_EQ(frame.size(), kFrameHeaderSize + payload.size());

  for (std::size_t length = 0; length < kFrameHeaderSize; ++length) {
    const FrameOutcome outcome = decode_frame(std::string_view(frame).substr(0, length));
    if (outcome.status != ProtocolStatus::TruncatedHeader) {
      CF_FAIL("prefix length " + std::to_string(length) + " was reported as " +
              std::string(to_string(outcome.status)) + " instead of TRUNCATED_HEADER");
    }
    CF_EXPECT_EQ(outcome.consumed, std::size_t{0});
    CF_EXPECT(outcome.payload.empty());
  }

  for (std::size_t length = kFrameHeaderSize; length < frame.size(); ++length) {
    const FrameOutcome outcome = decode_frame(std::string_view(frame).substr(0, length));
    if (outcome.status != ProtocolStatus::TruncatedPayload) {
      CF_FAIL("payload prefix length " + std::to_string(length) + " was reported as " +
              std::string(to_string(outcome.status)) + " instead of TRUNCATED_PAYLOAD");
    }
    CF_EXPECT_EQ(outcome.header.payload_length, static_cast<std::uint32_t>(payload.size()));
    CF_EXPECT_EQ(outcome.consumed, std::size_t{0});
  }

  CF_EXPECT_EQ(decode_frame(frame).status, ProtocolStatus::Ok);
}

CF_TEST(frame_rejects_bad_magic_version_and_unknown_type) {
  const std::string frame = encode_frame(MessageType::Hello, 5, "hello-payload");
  CF_EXPECT(!frame.empty());

  std::string bad_magic = frame;
  bad_magic[0] = static_cast<char>(0x00);
  const FrameOutcome magic_outcome = decode_frame(bad_magic);
  CF_EXPECT_EQ(magic_outcome.status, ProtocolStatus::BadMagic);
  CF_EXPECT_EQ(magic_outcome.consumed, std::size_t{0});

  std::string bad_version = frame;
  bad_version[4] = static_cast<char>(0x02);
  const FrameOutcome version_outcome = decode_frame(bad_version);
  CF_EXPECT_EQ(version_outcome.status, ProtocolStatus::UnsupportedVersion);

  std::string unknown_type = frame;
  unknown_type[6] = static_cast<char>(0x11);  // 17
  const FrameOutcome type_outcome = decode_frame(unknown_type);
  CF_EXPECT_EQ(type_outcome.status, ProtocolStatus::UnknownMessage);

  // A type that is declared (INVALID = 0) passes framing; only the payload
  // codec may refuse it.
  std::string invalid_type = frame;
  invalid_type[6] = static_cast<char>(0x00);
  const FrameOutcome invalid_outcome = decode_frame(invalid_type);
  CF_EXPECT_EQ(invalid_outcome.status, ProtocolStatus::ChecksumMismatch);

  const std::string declared_invalid = encode_frame(MessageType::Invalid, 5, "hello-payload");
  CF_EXPECT(!declared_invalid.empty());
  const FrameOutcome declared_outcome = decode_frame(declared_invalid);
  CF_EXPECT_EQ(declared_outcome.status, ProtocolStatus::Ok);
  CF_EXPECT_EQ(declared_outcome.header.type, MessageType::Invalid);
}

CF_TEST(frame_every_single_bit_flip_is_rejected) {
  const std::string payload = "a-payload-long-enough-to-flip";
  const std::string frame = encode_frame(MessageType::SnapshotRequest, 0xDEADBEEFull, payload);
  CF_EXPECT_EQ(frame.size(), kFrameHeaderSize + payload.size());
  CF_EXPECT_EQ(decode_frame(frame).status, ProtocolStatus::Ok);

  std::size_t checked = 0;
  for (std::size_t index = 0; index < frame.size(); ++index) {
    for (unsigned bit = 0; bit < 8u; ++bit) {
      const std::string corrupted = flip_bit(frame, index, bit);
      const FrameOutcome outcome = decode_frame(corrupted);
      const ProtocolStatus expected = expected_status_for_corruption(corrupted, kMaxFramePayloadBytes);
      if (outcome.status != expected) {
        CF_FAIL("flip of byte " + std::to_string(index) + " bit " + std::to_string(bit) +
                " reported " + std::string(to_string(outcome.status)) + ", expected " +
                std::string(to_string(expected)));
      }
      if (outcome.status == ProtocolStatus::Ok) {
        CF_FAIL("flip of byte " + std::to_string(index) + " bit " + std::to_string(bit) +
                " was accepted");
      }
      CF_EXPECT_EQ(outcome.consumed, std::size_t{0});
      ++checked;
    }
  }
  CF_EXPECT_EQ(checked, frame.size() * 8u);

  // A flip inside the payload never changes magic, version, type or length, so
  // it must be reported as a checksum mismatch.
  for (std::size_t index = kFrameHeaderSize; index < frame.size(); ++index) {
    for (unsigned bit = 0; bit < 8u; ++bit) {
      const FrameOutcome outcome = decode_frame(flip_bit(frame, index, bit));
      if (outcome.status != ProtocolStatus::ChecksumMismatch) {
        CF_FAIL("payload flip at byte " + std::to_string(index) + " bit " + std::to_string(bit) +
                " reported " + std::string(to_string(outcome.status)) +
                " instead of CHECKSUM_MISMATCH");
      }
    }
  }
  // A flip in the checksum field itself is also a checksum mismatch.
  for (std::size_t index = 24; index < kFrameHeaderSize; ++index) {
    for (unsigned bit = 0; bit < 8u; ++bit) {
      const FrameOutcome outcome = decode_frame(flip_bit(frame, index, bit));
      if (outcome.status != ProtocolStatus::ChecksumMismatch) {
        CF_FAIL("checksum-field flip at byte " + std::to_string(index) + " bit " +
                std::to_string(bit) + " reported " + std::string(to_string(outcome.status)) +
                " instead of CHECKSUM_MISMATCH");
      }
    }
  }
}

CF_TEST(frame_payload_bounds_are_enforced_before_allocation) {
  const std::string at_limit(static_cast<std::size_t>(kMaxFramePayloadBytes), 'x');
  const std::string above_limit(static_cast<std::size_t>(kMaxFramePayloadBytes) + 1u, 'x');
  const std::string above_absolute(static_cast<std::size_t>(kAbsoluteMaxFramePayloadBytes) + 1u, 'x');

  const std::string at_limit_frame = encode_frame(MessageType::PublishRequest, 1, at_limit);
  CF_EXPECT(!at_limit_frame.empty());
  CF_EXPECT_EQ(at_limit_frame.size(), kFrameHeaderSize + at_limit.size());
  CF_EXPECT_EQ(decode_frame(at_limit_frame).status, ProtocolStatus::Ok);

  CF_EXPECT(encode_frame(MessageType::PublishRequest, 1, above_limit).empty());
  CF_EXPECT(encode_frame(MessageType::PublishRequest, 1, above_absolute).empty());
  CF_EXPECT(encode_frame(MessageType::PublishRequest, 1, above_absolute, 0,
                         kAbsoluteMaxFramePayloadBytes)
                .empty());

  // A configured maximum above the absolute ceiling is clamped, never honoured.
  const std::string clamped =
      encode_frame(MessageType::PublishRequest, 1, above_limit, 0, kAbsoluteMaxFramePayloadBytes);
  CF_EXPECT(!clamped.empty());
  CF_EXPECT_EQ(decode_frame(clamped, kAbsoluteMaxFramePayloadBytes + 5u).status,
               ProtocolStatus::Ok);

  // A frame that declares more than the configured maximum is refused with the
  // typed status and never allocates the declared length.
  const FrameOutcome oversized = decode_frame(clamped, kMaxFramePayloadBytes - 1u);
  CF_EXPECT_EQ(oversized.status, ProtocolStatus::OversizedFrame);
  CF_EXPECT_EQ(oversized.header.payload_length, static_cast<std::uint32_t>(above_limit.size()));
  CF_EXPECT_EQ(oversized.consumed, std::size_t{0});
  CF_EXPECT(oversized.payload.empty());

  // A declared length of 0xFFFFFFFF is refused before the availability check.
  std::string hostile = clamped.substr(0, kFrameHeaderSize);
  hostile[20] = static_cast<char>(0xFF);
  hostile[21] = static_cast<char>(0xFF);
  hostile[22] = static_cast<char>(0xFF);
  hostile[23] = static_cast<char>(0xFF);
  const FrameOutcome hostile_outcome = decode_frame(hostile);
  CF_EXPECT_EQ(hostile_outcome.status, ProtocolStatus::OversizedFrame);
}

// ---------------------------------------------------------------------------
// Every message type round-trips
// ---------------------------------------------------------------------------

CF_TEST(all_message_types_round_trip_when_fully_populated) {
  expect_message_round_trip(full_hello(), &encode_hello, &decode_hello, MessageType::Hello,
                            "hello");
  expect_message_round_trip(full_hello_ack(), &encode_hello_ack, &decode_hello_ack,
                            MessageType::HelloAck, "hello_ack");
  expect_message_round_trip(full_register_publisher(), &encode_register_publisher,
                            &decode_register_publisher, MessageType::RegisterPublisher,
                            "register_publisher");
  expect_message_round_trip(full_register_ack(), &encode_register_ack, &decode_register_ack,
                            MessageType::RegisterAck, "register_ack");
  expect_message_round_trip(full_publish_request(), &encode_publish_request,
                            &decode_publish_request, MessageType::PublishRequest,
                            "publish_request");
  expect_message_round_trip(full_publish_result(), &encode_publish_result,
                            &decode_publish_result, MessageType::PublishResult,
                            "publish_result");
  expect_message_round_trip(full_heartbeat(), &encode_heartbeat, &decode_heartbeat,
                            MessageType::Heartbeat, "heartbeat");
  expect_message_round_trip(full_heartbeat_ack(), &encode_heartbeat_ack, &decode_heartbeat_ack,
                            MessageType::HeartbeatAck, "heartbeat_ack");
  expect_message_round_trip(full_snapshot_request(), &encode_snapshot_request,
                            &decode_snapshot_request, MessageType::SnapshotRequest,
                            "snapshot_request");
  expect_message_round_trip(full_snapshot_response(), &encode_snapshot_response,
                            &decode_snapshot_response, MessageType::SnapshotResponse,
                            "snapshot_response");
  expect_message_round_trip(full_validate_request(), &encode_validate_request,
                            &decode_validate_request, MessageType::ValidateSnapshotRequest,
                            "validate_snapshot_request");
  expect_message_round_trip(full_validate_response(), &encode_validate_response,
                            &decode_validate_response, MessageType::ValidateSnapshotResponse,
                            "validate_snapshot_response");
  expect_message_round_trip(full_revalidate_request(), &encode_revalidate_request,
                            &decode_revalidate_request, MessageType::RevalidateRequest,
                            "revalidate_request");
  expect_message_round_trip(full_revalidate_response(), &encode_revalidate_response,
                            &decode_revalidate_response, MessageType::RevalidateResponse,
                            "revalidate_response");
  expect_message_round_trip(full_shutdown(), &encode_shutdown, &decode_shutdown,
                            MessageType::Shutdown, "shutdown");
  expect_message_round_trip(full_error_message(), &encode_error, &decode_error,
                            MessageType::Error, "error");
}

CF_TEST(all_message_types_round_trip_without_optional_fields) {
  expect_message_round_trip(minimal_hello(), &encode_hello, &decode_hello, MessageType::Hello,
                            "minimal hello");
  expect_message_round_trip(minimal_hello_ack(), &encode_hello_ack, &decode_hello_ack,
                            MessageType::HelloAck, "minimal hello_ack");
  expect_message_round_trip(minimal_register_publisher(), &encode_register_publisher,
                            &decode_register_publisher, MessageType::RegisterPublisher,
                            "minimal register_publisher");
  expect_message_round_trip(minimal_register_ack(), &encode_register_ack, &decode_register_ack,
                            MessageType::RegisterAck, "minimal register_ack");
  expect_message_round_trip(minimal_publish_request(), &encode_publish_request,
                            &decode_publish_request, MessageType::PublishRequest,
                            "minimal publish_request");
  expect_message_round_trip(minimal_publish_result(), &encode_publish_result,
                            &decode_publish_result, MessageType::PublishResult,
                            "minimal publish_result");
  expect_message_round_trip(minimal_heartbeat(), &encode_heartbeat, &decode_heartbeat,
                            MessageType::Heartbeat, "minimal heartbeat");
  expect_message_round_trip(minimal_heartbeat_ack(), &encode_heartbeat_ack,
                            &decode_heartbeat_ack, MessageType::HeartbeatAck,
                            "minimal heartbeat_ack");
  expect_message_round_trip(full_snapshot_request(), &encode_snapshot_request,
                            &decode_snapshot_request, MessageType::SnapshotRequest,
                            "minimal snapshot_request");
  expect_message_round_trip(minimal_snapshot_response(), &encode_snapshot_response,
                            &decode_snapshot_response, MessageType::SnapshotResponse,
                            "minimal snapshot_response");
  expect_message_round_trip(minimal_validate_request(), &encode_validate_request,
                            &decode_validate_request, MessageType::ValidateSnapshotRequest,
                            "minimal validate_snapshot_request");
  expect_message_round_trip(minimal_validate_response(), &encode_validate_response,
                            &decode_validate_response, MessageType::ValidateSnapshotResponse,
                            "minimal validate_snapshot_response");
  expect_message_round_trip(minimal_revalidate_request(), &encode_revalidate_request,
                            &decode_revalidate_request, MessageType::RevalidateRequest,
                            "minimal revalidate_request");
  expect_message_round_trip(minimal_revalidate_response(), &encode_revalidate_response,
                            &decode_revalidate_response, MessageType::RevalidateResponse,
                            "minimal revalidate_response");
  expect_message_round_trip(minimal_shutdown(), &encode_shutdown, &decode_shutdown,
                            MessageType::Shutdown, "minimal shutdown");
  expect_message_round_trip(minimal_error_message(), &encode_error, &decode_error,
                            MessageType::Error, "minimal error");

  // Optionals really are absent in the minimal encodings.
  PublishRequestMessage publish;
  const DecodeOutcome outcome =
      decode_publish_request(encode_publish_request(minimal_publish_request()), publish);
  CF_EXPECT_EQ(outcome.status, ProtocolStatus::Ok);
  const MutationRequest& decoded = publish.request;
  CF_EXPECT(!decoded.authority.boot.has_value());
  CF_EXPECT(!decoded.authority.publisher.has_value());
  CF_EXPECT(!decoded.authority.rack.has_value());
  CF_EXPECT(!decoded.authority.rack_generation.has_value());
  CF_EXPECT(!decoded.expected_rack_generation.has_value());
  CF_EXPECT(!decoded.new_rack_generation.has_value());
  CF_EXPECT(!decoded.placement_domain.has_value());
  CF_EXPECT(!decoded.capacity_domain.has_value());
  CF_EXPECT(!decoded.failure_domain.has_value());
  CF_EXPECT(!decoded.network_domain.has_value());
  CF_EXPECT(!decoded.storage_domain.has_value());
  CF_EXPECT(!decoded.power_domain.has_value());
  CF_EXPECT(!decoded.cooling_domain.has_value());
  CF_EXPECT(!decoded.link_domain.has_value());
  CF_EXPECT(!decoded.constraint.has_value());
  CF_EXPECT(!decoded.link.has_value());
  CF_EXPECT(!decoded.link_id.has_value());
  CF_EXPECT(!decoded.target_topology_epoch.has_value());
  CF_EXPECT(decoded.rack_reference.endpoints.empty());
  CF_EXPECT(decoded.rack_reference.failure_domain_hints.empty());
  CF_EXPECT(decoded.rack_reference.composition.accelerators.empty());
  CF_EXPECT(!decoded.rack_reference.publisher.has_value());
  CF_EXPECT(decoded.readiness_contract.mandatory_racks.empty());
  CF_EXPECT(decoded.readiness_contract.required_placement_domain_classes.empty());
  CF_EXPECT(decoded.readiness_contract.required_failure_domain_classes.empty());
}

// ---------------------------------------------------------------------------
// Shared codecs
// ---------------------------------------------------------------------------

CF_TEST(shared_mutation_request_codec_round_trips) {
  const MutationRequest original = full_mutation_request();
  MutationRequest decoded;
  expect_codec_round_trip(original, &encode_mutation_request, &decode_mutation_request,
                          "mutation_request");

  ByteWriter writer;
  CF_EXPECT(encode_mutation_request(original, writer));
  ByteReader reader(writer.data());
  CF_EXPECT_EQ(decode_mutation_request(reader, decoded).status, ProtocolStatus::Ok);
  CF_EXPECT(reader.exhausted());

  CF_EXPECT_EQ(decoded.kind, original.kind);
  CF_EXPECT_EQ(decoded.cluster, original.cluster);
  CF_EXPECT_EQ(decoded.authority, original.authority);
  CF_EXPECT_EQ(decoded.evidence, original.evidence);
  CF_EXPECT_EQ(decoded.reason, original.reason);
  CF_EXPECT_EQ(decoded.requested_at, original.requested_at);
  CF_EXPECT_EQ(decoded.declared_lifecycle, original.declared_lifecycle);
  CF_EXPECT_EQ(decoded.readiness_contract, original.readiness_contract);
  CF_EXPECT_EQ(decoded.rack_reference, original.rack_reference);
  CF_EXPECT_EQ(decoded.membership, original.membership);
  CF_EXPECT_EQ(decoded.expected_rack_generation, original.expected_rack_generation);
  CF_EXPECT_EQ(decoded.new_rack_generation, original.new_rack_generation);
  CF_EXPECT_EQ(decoded.placement_domain, original.placement_domain);
  CF_EXPECT_EQ(decoded.capacity_domain, original.capacity_domain);
  CF_EXPECT_EQ(decoded.failure_domain, original.failure_domain);
  CF_EXPECT_EQ(decoded.network_domain, original.network_domain);
  CF_EXPECT_EQ(decoded.storage_domain, original.storage_domain);
  CF_EXPECT_EQ(decoded.power_domain, original.power_domain);
  CF_EXPECT_EQ(decoded.cooling_domain, original.cooling_domain);
  CF_EXPECT_EQ(decoded.link_domain, original.link_domain);
  CF_EXPECT_EQ(decoded.constraint, original.constraint);
  CF_EXPECT_EQ(decoded.link, original.link);
  CF_EXPECT_EQ(decoded.link_id, original.link_id);
  CF_EXPECT_EQ(decoded.target_topology_epoch, original.target_topology_epoch);
  CF_EXPECT_EQ(decoded.topology_reason, original.topology_reason);
  CF_EXPECT_EQ(decoded.health, original.health);
  CF_EXPECT_EQ(decoded.reachability, original.reachability);
  CF_EXPECT_EQ(decoded.evidence_selector, original.evidence_selector);
  // The documented REVALIDATE_RECOVERED_STATE rack list is part of the request
  // and must survive the shared codec.
  CF_EXPECT_EQ(decoded.racks.size(), original.racks.size());
  CF_EXPECT_EQ(decoded.racks, original.racks);
}

CF_TEST(shared_mutation_result_codec_round_trips) {
  const MutationResult original = full_mutation_result();
  MutationResult decoded;
  expect_codec_round_trip(original, &encode_mutation_result, &decode_mutation_result,
                          "mutation_result");

  ByteWriter writer;
  CF_EXPECT(encode_mutation_result(original, writer));
  ByteReader reader(writer.data());
  CF_EXPECT_EQ(decode_mutation_result(reader, decoded).status, ProtocolStatus::Ok);
  CF_EXPECT_EQ(decoded.outcome, original.outcome);
  CF_EXPECT_EQ(decoded.reason, original.reason);
  CF_EXPECT_EQ(decoded.error.category, original.error.category);
  CF_EXPECT_EQ(decoded.error.stage, original.error.stage);
  CF_EXPECT_EQ(decoded.error.subject, original.error.subject);
  CF_EXPECT_EQ(decoded.error.reason, original.error.reason);
  CF_EXPECT_EQ(decoded.error.expected, original.error.expected);
  CF_EXPECT_EQ(decoded.error.current, original.error.current);
  CF_EXPECT_EQ(decoded.error.message, original.error.message);
  CF_EXPECT_EQ(decoded.explanation, original.explanation);
  CF_EXPECT_EQ(decoded.cluster_epoch, original.cluster_epoch);
  CF_EXPECT_EQ(decoded.coordinator_epoch, original.coordinator_epoch);
  CF_EXPECT_EQ(decoded.cluster_generation, original.cluster_generation);
  CF_EXPECT_EQ(decoded.membership_generation, original.membership_generation);
  CF_EXPECT_EQ(decoded.topology_epoch, original.topology_epoch);
  CF_EXPECT_EQ(decoded.topology_generation, original.topology_generation);
  CF_EXPECT_EQ(decoded.connectivity_generation, original.connectivity_generation);
  CF_EXPECT_EQ(decoded.health_generation, original.health_generation);
  CF_EXPECT_EQ(decoded.constraint_generation, original.constraint_generation);
  CF_EXPECT_EQ(decoded.snapshot_generation, original.snapshot_generation);
  CF_EXPECT_EQ(decoded.publication_generation, original.publication_generation);
  CF_EXPECT_EQ(decoded.lifecycle, original.lifecycle);
}

CF_TEST(shared_snapshot_view_codec_round_trips) {
  const SnapshotView original = full_snapshot_view();
  SnapshotView decoded;
  expect_codec_round_trip(original, &encode_snapshot_view, &decode_snapshot_view, "snapshot_view");

  ByteWriter writer;
  CF_EXPECT(encode_snapshot_view(original, writer));
  ByteReader reader(writer.data());
  CF_EXPECT_EQ(decode_snapshot_view(reader, decoded).status, ProtocolStatus::Ok);
  CF_EXPECT_EQ(decoded, original);
}

CF_TEST(shared_structured_error_codec_round_trips) {
  const StructuredError original = full_error();
  StructuredError decoded;
  expect_codec_round_trip(original, &encode_structured_error, &decode_structured_error,
                          "structured_error");

  ByteWriter writer;
  CF_EXPECT(encode_structured_error(original, writer));
  ByteReader reader(writer.data());
  CF_EXPECT_EQ(decode_structured_error(reader, decoded).status, ProtocolStatus::Ok);
  expect_same_error(decoded, original, "structured_error");

  // An empty error is a legal value: category NONE is not a failure.
  StructuredError empty;
  StructuredError empty_decoded;
  expect_codec_round_trip(empty, &encode_structured_error, &decode_structured_error,
                          "empty structured_error");
  ByteWriter empty_writer;
  CF_EXPECT(encode_structured_error(empty, empty_writer));
  ByteReader empty_reader(empty_writer.data());
  CF_EXPECT_EQ(decode_structured_error(empty_reader, empty_decoded).status, ProtocolStatus::Ok);
  expect_same_error(empty_decoded, empty, "empty structured_error");
  CF_EXPECT_EQ(empty_decoded.category, ErrorCategory::None);
  CF_EXPECT(empty_decoded.ok());
}

// ---------------------------------------------------------------------------
// Adversarial payloads
// ---------------------------------------------------------------------------

CF_TEST(adversarial_length_prefix_is_rejected_without_allocation) {
  // A HELLO payload is: version tag, protocol version, cluster, boot, role,
  // build. Only the cluster length prefix is corrupted below.
  auto hello_prefix = [](std::uint32_t declared) {
    std::string payload;
    put_u16(payload, kProtocolVersion);
    put_u16(payload, kProtocolVersion);
    put_u32(payload, declared);
    payload += "ab";
    return payload;
  };

  HelloMessage out;
  CF_EXPECT_EQ(decode_hello(hello_prefix(0xFFFFFFFFu), out).status, ProtocolStatus::AbsurdLength);
  CF_EXPECT_EQ(decode_hello(hello_prefix(static_cast<std::uint32_t>(kMaxEncodedStringBytes + 1u)),
                            out)
                   .status,
               ProtocolStatus::AbsurdLength);
  CF_EXPECT_EQ(decode_hello(hello_prefix(static_cast<std::uint32_t>(kMaxEncodedStringBytes)), out)
                   .status,
               ProtocolStatus::TruncatedPayload);

  std::string truncated_prefix;
  put_u16(truncated_prefix, kProtocolVersion);
  truncated_prefix.push_back(static_cast<char>(0x01));
  CF_EXPECT_EQ(decode_hello(truncated_prefix, out).status, ProtocolStatus::TruncatedPayload);

  std::string empty_payload;
  CF_EXPECT_EQ(decode_hello(empty_payload, out).status, ProtocolStatus::TruncatedPayload);

  // The same length prefix inside a shared codec is bounded identically.
  std::string view_prefix;
  put_u16(view_prefix, kProtocolVersion);
  put_u32(view_prefix, 0xFFFFFFFFu);
  ByteReader view_reader(view_prefix);
  SnapshotView view;
  CF_EXPECT_EQ(decode_snapshot_view(view_reader, view).status, ProtocolStatus::AbsurdLength);
}

CF_TEST(adversarial_counts_above_the_limits_are_rejected) {
  std::string payload;
  put_u16(payload, kProtocolVersion);
  put_text(payload, "cluster-a");
  for (int index = 0; index < 4; ++index) {
    put_u64(payload, 1);
  }
  for (int index = 0; index < 4; ++index) {
    payload.push_back(static_cast<char>(0x00));
  }
  put_u64(payload, 1);
  put_u32(payload, 0xFFFFFFFFu);
  RevalidateRequestMessage out;
  CF_EXPECT_EQ(decode_revalidate_request(payload, out).status, ProtocolStatus::AbsurdLength);

  // One above the declared bound of the rack vector.
  std::string above_bound;
  put_u16(above_bound, kProtocolVersion);
  put_text(above_bound, "cluster-a");
  for (int index = 0; index < 4; ++index) {
    put_u64(above_bound, 1);
  }
  for (int index = 0; index < 4; ++index) {
    above_bound.push_back(static_cast<char>(0x00));
  }
  put_u64(above_bound, 1);
  put_u32(above_bound, static_cast<std::uint32_t>(kMaxRacksPerCluster) + 1u);
  CF_EXPECT_EQ(decode_revalidate_request(above_bound, out).status, ProtocolStatus::AbsurdLength);

  // A count that fits the bound but not the remaining bytes is still refused
  // before any reservation.
  std::string beyond_remaining;
  put_u16(beyond_remaining, kProtocolVersion);
  put_text(beyond_remaining, "cluster-a");
  for (int index = 0; index < 4; ++index) {
    put_u64(beyond_remaining, 1);
  }
  for (int index = 0; index < 4; ++index) {
    beyond_remaining.push_back(static_cast<char>(0x00));
  }
  put_u64(beyond_remaining, 1);
  put_u32(beyond_remaining, 1000u);
  CF_EXPECT_EQ(decode_revalidate_request(beyond_remaining, out).status,
               ProtocolStatus::AbsurdLength);
}

CF_TEST(adversarial_identities_enums_and_markers_are_rejected) {
  // Invalid identity characters.
  std::string bad_identity;
  put_u16(bad_identity, kProtocolVersion);
  put_u16(bad_identity, kProtocolVersion);
  put_text(bad_identity, "bad id");
  put_text(bad_identity, "boot-a");
  put_text(bad_identity, "role");
  put_text(bad_identity, "build");
  HelloMessage hello;
  CF_EXPECT_EQ(decode_hello(bad_identity, hello).status, ProtocolStatus::InvalidIdentity);

  // Empty identity.
  std::string empty_identity;
  put_u16(empty_identity, kProtocolVersion);
  put_u16(empty_identity, kProtocolVersion);
  put_text(empty_identity, "");
  put_text(empty_identity, "boot-a");
  put_text(empty_identity, "role");
  put_text(empty_identity, "build");
  CF_EXPECT_EQ(decode_hello(empty_identity, hello).status, ProtocolStatus::InvalidIdentity);

  // Enum value out of range.
  std::string bad_enum;
  put_u16(bad_enum, kProtocolVersion);
  put_u16(bad_enum, kProtocolVersion);
  put_text(bad_enum, "cluster-a");
  for (int index = 0; index < 4; ++index) {
    put_u64(bad_enum, 1);
  }
  bad_enum.push_back(static_cast<char>(0x09));  // lifecycle 9 > RETIRED (7)
  bad_enum.push_back(static_cast<char>(0x01));
  bad_enum.push_back(static_cast<char>(0x00));
  put_text(bad_enum, "detail");
  HelloAckMessage ack;
  CF_EXPECT_EQ(decode_hello_ack(bad_enum, ack).status, ProtocolStatus::InvalidEnum);

  // Optional marker greater than one.
  std::string bad_marker;
  put_u16(bad_marker, kProtocolVersion);
  bad_marker.push_back(static_cast<char>(static_cast<std::uint8_t>(MutationKind::AddRack)));
  put_text(bad_marker, "cluster-a");
  for (int index = 0; index < 4; ++index) {
    put_u64(bad_marker, 1);
  }
  bad_marker.push_back(static_cast<char>(0x02));  // boot marker must be 0 or 1
  ByteReader marker_reader(bad_marker);
  MutationRequest request;
  CF_EXPECT_EQ(decode_mutation_request(marker_reader, request).status,
               ProtocolStatus::MalformedPayload);

  // Boolean marker greater than one.
  std::string bad_bool;
  put_u16(bad_bool, kProtocolVersion);
  bad_bool.push_back(static_cast<char>(0x02));
  RegisterAckMessage register_ack;
  CF_EXPECT_EQ(decode_register_ack(bad_bool, register_ack).status,
               ProtocolStatus::MalformedPayload);

  // Unsupported payload version tag.
  std::string bad_version;
  put_u16(bad_version, static_cast<std::uint16_t>(kProtocolVersion + 1u));
  ShutdownMessage shutdown;
  CF_EXPECT_EQ(decode_shutdown(bad_version, shutdown).status, ProtocolStatus::UnsupportedVersion);

  // Trailing bytes after a complete payload.
  const std::string shutdown_payload = encode_shutdown(full_shutdown());
  CF_EXPECT_EQ(decode_shutdown(shutdown_payload + "x", shutdown).status,
               ProtocolStatus::TrailingGarbage);
}

CF_TEST(adversarial_negative_and_huge_counters_do_not_allocate) {
  // A negative timestamp survives the codec exactly; it is never coerced and
  // never used to size an allocation.
  MutationRequest request = minimal_mutation_request();
  request.requested_at = Timestamp::from_unix_millis(-1);
  MutationRequest decoded;
  ByteWriter writer;
  CF_EXPECT(encode_mutation_request(request, writer));
  ByteReader reader(writer.data());
  CF_EXPECT_EQ(decode_mutation_request(reader, decoded).status, ProtocolStatus::Ok);
  CF_EXPECT_EQ(decoded.requested_at, Timestamp::from_unix_millis(-1));
  CF_EXPECT(!decoded.requested_at.known());

  // The largest possible counter value round-trips as a raw counter and is
  // never interpreted as a count.
  SnapshotView view;
  view.cluster = make_id<ClusterId>("cluster-a");
  view.cluster_generation = ClusterGeneration::from_raw(~std::uint64_t{0});
  view.membership_generation = MembershipGeneration::from_raw(~std::uint64_t{0});
  view.rack_count = 0;
  ByteWriter view_writer;
  CF_EXPECT(encode_snapshot_view(view, view_writer));
  ByteReader view_reader(view_writer.data());
  SnapshotView decoded_view;
  CF_EXPECT_EQ(decode_snapshot_view(view_reader, decoded_view).status, ProtocolStatus::Ok);
  CF_EXPECT_EQ(decoded_view.cluster_generation.value(), ~std::uint64_t{0});
  CF_EXPECT_EQ(decoded_view.membership_generation.value(), ~std::uint64_t{0});
}

CF_TEST(byte_writer_and_reader_are_bounded_and_little_endian) {
  ByteWriter writer;
  writer.u8(0x12);
  writer.boolean(true);
  writer.u16(0x3456);
  writer.u32(0x789ABCDEu);
  writer.u64(0x0123456789ABCDEFull);
  writer.i64(-2);
  writer.f64(1.5);
  writer.optional_present(false);
  writer.text("ab");
  CF_EXPECT(writer.ok());
  CF_EXPECT_EQ(writer.status(), ProtocolStatus::Ok);

  const std::string& bytes = writer.data();
  CF_EXPECT_EQ(byte_at(bytes, 0), std::uint8_t{0x12});
  CF_EXPECT_EQ(byte_at(bytes, 1), std::uint8_t{0x01});
  CF_EXPECT_EQ(read_le16(bytes, 2), std::uint16_t{0x3456});
  CF_EXPECT_EQ(read_le32(bytes, 4), 0x789ABCDEu);
  CF_EXPECT_EQ(read_le64(bytes, 8), 0x0123456789ABCDEFull);
  CF_EXPECT_EQ(read_le64(bytes, 16), ~std::uint64_t{0} - 1u);
  CF_EXPECT_EQ(read_le64(bytes, 24), std::bit_cast<std::uint64_t>(1.5));
  CF_EXPECT_EQ(byte_at(bytes, 32), std::uint8_t{0x00});
  CF_EXPECT_EQ(read_le32(bytes, 33), 2u);
  CF_EXPECT_EQ(bytes.substr(37), "ab");
  CF_EXPECT_EQ(bytes.size(), std::size_t{39});

  ByteReader reader(bytes);
  CF_EXPECT_EQ(reader.u8(), std::optional<std::uint8_t>{std::uint8_t{0x12}});
  CF_EXPECT_EQ(reader.boolean(), std::optional<bool>{true});
  CF_EXPECT_EQ(reader.u16(), std::optional<std::uint16_t>{std::uint16_t{0x3456}});
  CF_EXPECT_EQ(reader.u32(), std::optional<std::uint32_t>{0x789ABCDEu});
  CF_EXPECT_EQ(reader.u64(), std::optional<std::uint64_t>{0x0123456789ABCDEFull});
  CF_EXPECT_EQ(reader.i64(), std::optional<std::int64_t>{-2});
  const std::optional<double> value = reader.f64();
  CF_EXPECT(value.has_value());
  CF_EXPECT_EQ(*value, 1.5);
  CF_EXPECT_EQ(reader.u8(), std::optional<std::uint8_t>{std::uint8_t{0x00}});
  CF_EXPECT_EQ(reader.text(), std::optional<std::string>{"ab"});
  CF_EXPECT(reader.ok());
  CF_EXPECT(reader.exhausted());
  CF_EXPECT_EQ(reader.remaining(), std::size_t{0});
  CF_EXPECT(!reader.u8().has_value());
  CF_EXPECT_EQ(reader.status(), ProtocolStatus::TruncatedPayload);

  ByteWriter bounded;
  bounded.text(std::string(kMaxEncodedStringBytes + 1u, 'a'));
  CF_EXPECT(!bounded.ok());
  CF_EXPECT_EQ(bounded.status(), ProtocolStatus::AbsurdLength);
  CF_EXPECT(bounded.data().empty());

  ByteReader empty(std::string_view{});
  CF_EXPECT(!empty.u16().has_value());
  CF_EXPECT_EQ(empty.status(), ProtocolStatus::TruncatedPayload);
  CF_EXPECT(empty.exhausted());
}

CF_TEST(message_type_text_round_trips_for_every_declared_type) {
  const MessageType declared[] = {
      MessageType::Invalid,          MessageType::Hello,
      MessageType::HelloAck,         MessageType::RegisterPublisher,
      MessageType::RegisterAck,      MessageType::PublishRequest,
      MessageType::PublishResult,    MessageType::Heartbeat,
      MessageType::HeartbeatAck,     MessageType::SnapshotRequest,
      MessageType::SnapshotResponse, MessageType::ValidateSnapshotRequest,
      MessageType::ValidateSnapshotResponse, MessageType::RevalidateRequest,
      MessageType::RevalidateResponse, MessageType::Shutdown,
      MessageType::Error};
  static_assert(sizeof(declared) / sizeof(declared[0]) == 17,
                "MessageType declares 17 enumerators");
  for (const MessageType type : declared) {
    const std::string_view text = to_string(type);
    CF_EXPECT(!text.empty());
    CF_EXPECT_NE(text, std::string_view{"UNKNOWN"});
    const std::optional<MessageType> parsed = message_type_from_string(text);
    CF_EXPECT(parsed.has_value());
    CF_EXPECT_EQ(*parsed, type);
  }
  CF_EXPECT(!message_type_from_string("NOT_A_MESSAGE").has_value());
}

int main() { return cf_test::run("test_protocol"); }
