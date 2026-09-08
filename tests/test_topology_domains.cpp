// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Inter-rack topology, the eight domain classes, capacity quantities,
// constraints and topology supersession.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "harness.hpp"

#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

namespace {

[[nodiscard]] ClusterId make_cluster(const char* text) { return *ClusterId::parse(text); }
[[nodiscard]] RackId make_rack(const char* text) { return *RackId::parse(text); }
[[nodiscard]] RackPublisherId make_publisher(const char* text) {
  return *RackPublisherId::parse(text);
}
[[nodiscard]] RackAgentBootId make_boot(const char* text) {
  return *RackAgentBootId::parse(text);
}

class Engine {
 public:
  explicit Engine(ReadinessContract contract = ReadinessContract::permissive())
      : store_(&own_store_) {
    config_.cluster = make_cluster("cluster-1");
    config_.readiness_contract = contract;
    coordinator_ = std::make_unique<ClusterCoordinator>(config_, store_, &clock_);
    const CoordinatorStartOutcome started = coordinator_->start();
    if (!started.ok) {
      CF_FAIL("coordinator failed to start: status " +
              std::string(to_string(started.status)) + " message " + started.error.message);
    }
  }

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  ~Engine() = default;

  [[nodiscard]] ClusterCoordinator& coordinator() { return *coordinator_; }
  [[nodiscard]] const ClusterId& cluster() const { return config_.cluster; }
  [[nodiscard]] ClusterState state() { return coordinator_->state_copy(); }
  [[nodiscard]] Timestamp now() const { return clock_.now(); }

  [[nodiscard]] EvidenceStamp evidence(std::int64_t ttl_millis = 60'000) const {
    return EvidenceStamp::make(EvidenceProvenance::Measured, clock_.now(), ttl_millis);
  }

  [[nodiscard]] MutationResult submit(const MutationRequest& request) {
    return coordinator_->submit(request);
  }

  [[nodiscard]] MutationRequest base(MutationKind kind) {
    MutationRequest request;
    request.kind = kind;
    request.cluster = cluster();
    const ClusterState current = state();
    request.authority.cluster_epoch = current.epoch;
    request.authority.coordinator_epoch = current.coordinator_epoch;
    request.authority.topology_epoch = current.topology_epoch;
    request.authority.topology_generation = current.topology_generation;
    request.requested_at = now();
    request.evidence = evidence();
    return request;
  }

 private:
  CoordinatorConfig config_;
  ManualClock clock_;
  MemoryPersistenceStore own_store_;
  PersistenceStore* store_ = nullptr;
  std::unique_ptr<ClusterCoordinator> coordinator_;
};

struct RackFixture {
  RackId rack;
  RackPublisherId publisher;
  RackAgentBootId boot;
  RackGeneration generation;
  PublicationGeneration publication;
};

void declare_cluster(Engine& engine) {
  MutationRequest request = engine.base(MutationKind::DeclareCluster);
  request.authority.cluster_epoch = ClusterEpoch::from_raw(1);
  request.declared_lifecycle = ClusterLifecycle::Declared;
  request.readiness_contract = ReadinessContract::permissive();
  CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
}

[[nodiscard]] RackFixture add_rack(Engine& engine, const char* rack_text, const char* boot_text) {
  RackFixture fixture;
  fixture.rack = make_rack(rack_text);
  fixture.publisher = make_publisher("publisher-1");
  fixture.boot = make_boot(boot_text);
  fixture.generation = RackGeneration::from_raw(1);
  fixture.publication = PublicationGeneration::from_raw(1);

  MutationRequest request = engine.base(MutationKind::AddRack);
  request.authority.rack = fixture.rack;
  request.authority.publisher = fixture.publisher;
  request.authority.boot = fixture.boot;
  request.authority.rack_generation = fixture.generation;
  request.authority.publication = fixture.publication;
  RackReference reference;
  reference.rack = fixture.rack;
  reference.generation = fixture.generation;
  reference.rack_lifecycle = RackLifecycleState::Ready;
  reference.currentness = RackCurrentness::Current;
  reference.evidence = request.evidence;
  reference.publisher = fixture.publisher;
  reference.publication = fixture.publication;
  reference.boot = fixture.boot;
  reference.health = HealthState::Healthy;
  reference.origin_label = "rack-fabric:test";
  request.rack_reference = reference;
  CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
  return fixture;
}

[[nodiscard]] InterRackLink make_link(const char* id, const RackId& source,
                                      const RackId& destination, TopologyEpoch epoch,
                                      const EvidenceStamp& stamp, std::uint64_t generation) {
  InterRackLink link;
  link.id = *InterRackLinkId::parse(id);
  link.source = source;
  link.destination = destination;
  link.direction = LinkDirection::Bidirectional;
  link.connectivity = ConnectivityClass::DirectFabric;
  link.reachability = Reachability::Reachable;
  link.health = HealthState::Healthy;
  link.header.generation = DomainGeneration::from_raw(generation);
  link.header.evidence = stamp;
  link.topology_epoch = epoch;
  return link;
}

[[nodiscard]] MutationResult publish_link(Engine& engine, const InterRackLink& link) {
  MutationRequest request = engine.base(MutationKind::PublishInterRackLink);
  request.link = link;
  return engine.submit(request);
}

[[nodiscard]] MutationRequest link_request(Engine& engine, const InterRackLink& link) {
  MutationRequest request = engine.base(MutationKind::PublishInterRackLink);
  request.link = link;
  return request;
}

[[nodiscard]] CapacityQuantity quantity(std::string unit, std::optional<double> value,
                                        EvidenceProvenance provenance, bool aggregated = false) {
  CapacityQuantity entry;
  entry.unit = std::move(unit);
  entry.value = value;
  entry.provenance = provenance;
  entry.aggregated = aggregated;
  return entry;
}

}  // namespace

// ---------------------------------------------------------------------------
// Inter-rack links
// ---------------------------------------------------------------------------

CF_TEST(topology_link_publish_and_withdraw_round_trip) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture first = add_rack(engine, "rack-a", "boot-1");
  const RackFixture second = add_rack(engine, "rack-b", "boot-2");
  const ClusterState before = engine.state();

  const MutationResult accepted =
      publish_link(engine, make_link("link-a-b", first.rack, second.rack, before.topology_epoch,
                                     engine.evidence(), 1));
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.links.size(), std::size_t{1});
  const InterRackLink& stored = published.links.begin()->second;
  CF_EXPECT_EQ(stored.source, first.rack);
  CF_EXPECT_EQ(stored.destination, second.rack);
  CF_EXPECT_EQ(stored.direction, LinkDirection::Bidirectional);
  CF_EXPECT_EQ(stored.connectivity, ConnectivityClass::DirectFabric);
  CF_EXPECT_EQ(stored.topology_epoch, published.topology_epoch);
  CF_EXPECT_EQ(stored.topology_generation, before.topology_generation);
  CF_EXPECT_EQ(stored.header.cluster_epoch, published.epoch);
  CF_EXPECT_EQ(stored.header.coordinator_epoch, published.coordinator_epoch);
  CF_EXPECT_EQ(published.topology_generation, *before.topology_generation.next());
  CF_EXPECT_EQ(published.connectivity_generation, *before.connectivity_generation.next());

  // An identical repeat is idempotent.
  MutationRequest repeat = link_request(engine, stored);
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  MutationRequest withdraw = engine.base(MutationKind::WithdrawInterRackLink);
  withdraw.link_id = *InterRackLinkId::parse("link-a-b");
  const MutationResult withdrawn = engine.submit(withdraw);
  CF_EXPECT_EQ(withdrawn.outcome, MutationOutcome::Accepted);
  const ClusterState after = engine.state();
  CF_EXPECT_EQ(after.links.size(), std::size_t{0});
  CF_EXPECT_EQ(after.topology_generation, *published.topology_generation.next());
  CF_EXPECT_EQ(after.connectivity_generation, *published.connectivity_generation.next());

  // Withdrawing a link that does not exist is NO_CHANGE, not a rejection.
  const MutationResult absent = engine.submit(withdraw);
  CF_EXPECT_EQ(absent.outcome, MutationOutcome::NoChange);
  CF_EXPECT_EQ(absent.reason, RejectionReason::None);
  CF_EXPECT_EQ(engine.state().links.size(), std::size_t{0});
}

CF_TEST(topology_link_validation_rules) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture first = add_rack(engine, "rack-a", "boot-1");
  const RackFixture second = add_rack(engine, "rack-b", "boot-2");
  const TopologyEpoch epoch = engine.state().topology_epoch;
  const EvidenceStamp stamp = engine.evidence();

  // A link connecting a rack to itself is refused before anything else.
  const MutationResult self =
      publish_link(engine, make_link("link-self", first.rack, first.rack, epoch, stamp, 1));
  CF_EXPECT_EQ(self.reason, RejectionReason::InvalidRelationship);

  // Direction UNKNOWN is never inferred.
  InterRackLink unknown_direction = make_link("link-dir", first.rack, second.rack, epoch, stamp, 1);
  unknown_direction.direction = LinkDirection::Unknown;
  CF_EXPECT_EQ(publish_link(engine, unknown_direction).reason,
               RejectionReason::InvalidRelationship);

  // Unknown source and destination racks.
  CF_EXPECT_EQ(publish_link(engine, make_link("link-src", make_rack("rack-missing"),
                                              second.rack, epoch, stamp, 1))
                   .reason,
               RejectionReason::UnknownRack);
  CF_EXPECT_EQ(publish_link(engine, make_link("link-dst", first.rack, make_rack("rack-missing"),
                                              epoch, stamp, 1))
                   .reason,
               RejectionReason::UnknownRack);

  // Unknown failure, network and link domains.
  InterRackLink unknown_failure =
      make_link("link-failure", first.rack, second.rack, epoch, stamp, 1);
  unknown_failure.failure_domains = {*FailureDomainId::parse("failure-missing")};
  CF_EXPECT_EQ(publish_link(engine, unknown_failure).reason, RejectionReason::UnknownDomain);

  InterRackLink unknown_network =
      make_link("link-network", first.rack, second.rack, epoch, stamp, 1);
  unknown_network.network_domain = *NetworkDomainId::parse("network-missing");
  CF_EXPECT_EQ(publish_link(engine, unknown_network).reason, RejectionReason::UnknownDomain);

  InterRackLink unknown_link_domain =
      make_link("link-domain", first.rack, second.rack, epoch, stamp, 1);
  unknown_link_domain.link_domain = *LinkDomainId::parse("link-domain-missing");
  CF_EXPECT_EQ(publish_link(engine, unknown_link_domain).reason, RejectionReason::UnknownDomain);

  // A zero generation is UNKNOWN, never authoritative.
  CF_EXPECT_EQ(publish_link(engine, make_link("link-zero", first.rack, second.rack, epoch, stamp,
                                              0))
                   .reason,
               RejectionReason::InvalidRelationship);

  // A topology epoch mismatch is refused.
  CF_EXPECT_EQ(publish_link(engine, make_link("link-epoch", first.rack, second.rack,
                                              TopologyEpoch::from_raw(9), stamp, 1))
                   .reason,
               RejectionReason::StaleTopologyEpoch);

  // Accept one link, then exercise the generation rules on it.
  CF_EXPECT_EQ(publish_link(engine, make_link("link-a-b", first.rack, second.rack, epoch, stamp, 2))
                   .outcome,
               MutationOutcome::Accepted);

  InterRackLink conflicting = make_link("link-a-b", first.rack, second.rack, epoch, stamp, 2);
  conflicting.direction = LinkDirection::Unidirectional;
  const MutationResult conflict = publish_link(engine, conflicting);
  CF_EXPECT_EQ(conflict.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(conflict.reason, RejectionReason::Conflict);
  CF_EXPECT_EQ(conflict.error.reason, std::string("conflicting_link_publication"));

  const MutationResult stale =
      publish_link(engine, make_link("link-a-b", first.rack, second.rack, epoch, stamp, 1));
  CF_EXPECT_EQ(stale.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(stale.reason, RejectionReason::StalePublication);
  CF_EXPECT_EQ(stale.error.reason, std::string("stale_link_generation"));

  const ClusterState state = engine.state();
  CF_EXPECT_EQ(state.links.size(), std::size_t{1});
  CF_EXPECT_EQ(state.links.begin()->second.direction, LinkDirection::Bidirectional);
}

CF_TEST(topology_link_records_optional_domain_references) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture first = add_rack(engine, "rack-a", "boot-1");
  const RackFixture second = add_rack(engine, "rack-b", "boot-2");

  MutationRequest network = engine.base(MutationKind::PublishNetworkDomain);
  NetworkDomain network_domain;
  network_domain.id = *NetworkDomainId::parse("network-1");
  network_domain.connectivity = ConnectivityClass::SwitchedFabric;
  network_domain.racks = {first.rack, second.rack};
  network_domain.header.generation = DomainGeneration::from_raw(1);
  network_domain.header.evidence = network.evidence;
  network.network_domain = network_domain;
  CF_EXPECT_EQ(engine.submit(network).outcome, MutationOutcome::Accepted);

  MutationRequest link_domain_request = engine.base(MutationKind::PublishLinkDomain);
  LinkDomain link_domain;
  link_domain.id = *LinkDomainId::parse("plane-1");
  link_domain.racks = {first.rack, second.rack};
  link_domain.header.generation = DomainGeneration::from_raw(1);
  link_domain.header.evidence = link_domain_request.evidence;
  link_domain_request.link_domain = link_domain;
  CF_EXPECT_EQ(engine.submit(link_domain_request).outcome, MutationOutcome::Accepted);

  MutationRequest failure = engine.base(MutationKind::PublishFailureDomain);
  FailureDomain failure_domain;
  failure_domain.id = *FailureDomainId::parse("row-1");
  failure_domain.klass = FailureDomainClass::Row;
  failure_domain.racks = {first.rack, second.rack};
  failure_domain.header.generation = DomainGeneration::from_raw(1);
  failure_domain.header.evidence = failure.evidence;
  failure.failure_domain = failure_domain;
  CF_EXPECT_EQ(engine.submit(failure).outcome, MutationOutcome::Accepted);

  InterRackLink link = make_link("link-a-b", first.rack, second.rack,
                                 engine.state().topology_epoch, engine.evidence(), 1);
  link.network_domain = network_domain.id;
  link.link_domain = link_domain.id;
  link.failure_domains = {*FailureDomainId::parse("row-1"), *FailureDomainId::parse("row-1")};
  link.nominal_bandwidth_bps = 400'000'000'000ull;
  link.nominal_latency_nanos = 250;
  link.bandwidth_class = BandwidthClass::VeryHigh;
  link.latency_class = LatencyClass::VeryLow;
  link.hop_count = 1;
  const MutationResult accepted = publish_link(engine, link);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);

  const ClusterState published = engine.state();
  const InterRackLink& stored = published.links.begin()->second;
  CF_EXPECT_EQ(stored.network_domain, network_domain.id);
  CF_EXPECT_EQ(stored.link_domain, link_domain.id);
  CF_EXPECT_EQ(stored.failure_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(stored.nominal_bandwidth_bps, link.nominal_bandwidth_bps);
  CF_EXPECT_EQ(stored.nominal_latency_nanos, link.nominal_latency_nanos);
  CF_EXPECT_EQ(stored.hop_count, link.hop_count);
  CF_EXPECT_EQ(reachability_between(engine.state(), first.rack, second.rack),
               Reachability::Reachable);
}

// ---------------------------------------------------------------------------
// Domain classes
// ---------------------------------------------------------------------------

CF_TEST(topology_all_eight_domain_classes_publish) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "boot-1");
  const ClusterState before = engine.state();
  CF_EXPECT(before.domain_generations.all_known());

  {
    MutationRequest request = engine.base(MutationKind::PublishPlacementDomain);
    PlacementDomain domain;
    domain.id = *PlacementDomainId::parse("placement-1");
    domain.klass = PlacementDomainClass::LowLatencyFabric;
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.placement_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishCapacityDomain);
    CapacityDomain domain;
    domain.id = *CapacityDomainId::parse("capacity-1");
    domain.klass = CapacityDomainClass::AcceleratorPool;
    domain.racks = {rack.rack};
    domain.quantities = {quantity("devices", 8.0, EvidenceProvenance::Measured)};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.capacity_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishFailureDomain);
    FailureDomain domain;
    domain.id = *FailureDomainId::parse("failure-1");
    domain.klass = FailureDomainClass::PowerFeed;
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.failure_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishNetworkDomain);
    NetworkDomain domain;
    domain.id = *NetworkDomainId::parse("network-1");
    domain.connectivity = ConnectivityClass::Overlay;
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.network_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishStorageDomain);
    StorageDomain domain;
    domain.id = *StorageDomainId::parse("storage-1");
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.storage_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishPowerDomain);
    PowerDomain domain;
    domain.id = *PowerDomainId::parse("power-1");
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.power_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishCoolingDomain);
    CoolingDomain domain;
    domain.id = *CoolingDomainId::parse("cooling-1");
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.cooling_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishLinkDomain);
    LinkDomain domain;
    domain.id = *LinkDomainId::parse("link-domain-1");
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.link_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);
  }

  const ClusterState state = engine.state();
  CF_EXPECT_EQ(state.placement_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(state.capacity_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(state.failure_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(state.network_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(state.storage_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(state.power_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(state.cooling_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(state.link_domains.size(), std::size_t{1});
  CF_EXPECT_EQ(state.placement_domains.begin()->second.klass,
               PlacementDomainClass::LowLatencyFabric);
  CF_EXPECT_EQ(state.capacity_domains.begin()->second.klass,
               CapacityDomainClass::AcceleratorPool);
  CF_EXPECT_EQ(state.failure_domains.begin()->second.klass, FailureDomainClass::PowerFeed);
  CF_EXPECT_EQ(state.network_domains.begin()->second.connectivity, ConnectivityClass::Overlay);
  for (std::uint64_t expected = 2; expected <= 2; ++expected) {
    CF_EXPECT_EQ(state.domain_generations.placement, DomainGeneration::from_raw(expected));
    CF_EXPECT_EQ(state.domain_generations.capacity, DomainGeneration::from_raw(expected));
    CF_EXPECT_EQ(state.domain_generations.failure, DomainGeneration::from_raw(expected));
    CF_EXPECT_EQ(state.domain_generations.network, DomainGeneration::from_raw(expected));
    CF_EXPECT_EQ(state.domain_generations.storage, DomainGeneration::from_raw(expected));
    CF_EXPECT_EQ(state.domain_generations.power, DomainGeneration::from_raw(expected));
    CF_EXPECT_EQ(state.domain_generations.cooling, DomainGeneration::from_raw(expected));
    CF_EXPECT_EQ(state.domain_generations.link, DomainGeneration::from_raw(expected));
  }
  CF_EXPECT_EQ(state.placement_domains.begin()->second.header.cluster_epoch, state.epoch);
  CF_EXPECT_EQ(state.placement_domains.begin()->second.header.coordinator_epoch,
               state.coordinator_epoch);
  CF_EXPECT(engine.coordinator().invariants().ok());
}

CF_TEST(topology_domain_class_validation_rejects_unknown) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "boot-1");

  {
    MutationRequest request = engine.base(MutationKind::PublishPlacementDomain);
    PlacementDomain domain;
    domain.id = *PlacementDomainId::parse("placement-1");
    domain.klass = PlacementDomainClass::Unknown;
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.placement_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::InvalidDomain);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishCapacityDomain);
    CapacityDomain domain;
    domain.id = *CapacityDomainId::parse("capacity-1");
    domain.klass = CapacityDomainClass::Unknown;
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.capacity_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::InvalidDomain);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishFailureDomain);
    FailureDomain domain;
    domain.id = *FailureDomainId::parse("failure-1");
    domain.klass = FailureDomainClass::Unknown;
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.failure_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::InvalidDomain);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishNetworkDomain);
    NetworkDomain domain;
    domain.id = *NetworkDomainId::parse("network-1");
    domain.connectivity = ConnectivityClass::Unknown;
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.network_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::InvalidDomain);
  }

  // A zero domain generation is UNKNOWN and refused for every class.
  {
    MutationRequest request = engine.base(MutationKind::PublishStorageDomain);
    StorageDomain domain;
    domain.id = *StorageDomainId::parse("storage-1");
    domain.racks = {rack.rack};
    domain.header.evidence = request.evidence;
    request.storage_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::InvalidDomain);
  }
  {
    MutationRequest request = engine.base(MutationKind::PublishLinkDomain);
    LinkDomain domain;
    domain.id = *LinkDomainId::parse("link-domain-1");
    domain.racks = {rack.rack};
    domain.header.evidence = request.evidence;
    request.link_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::InvalidDomain);
  }

  // An empty domain identity is refused for every class.
  {
    MutationRequest request = engine.base(MutationKind::PublishPowerDomain);
    PowerDomain domain;
    domain.racks = {rack.rack};
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.power_domain = domain;
    CF_EXPECT_EQ(engine.submit(request).reason, RejectionReason::InvalidDomain);
  }

  const ClusterState state = engine.state();
  CF_EXPECT_EQ(state.placement_domains.size(), std::size_t{0});
  CF_EXPECT_EQ(state.capacity_domains.size(), std::size_t{0});
  CF_EXPECT_EQ(state.failure_domains.size(), std::size_t{0});
  CF_EXPECT_EQ(state.network_domains.size(), std::size_t{0});
  CF_EXPECT_EQ(state.storage_domains.size(), std::size_t{0});
  CF_EXPECT_EQ(state.power_domains.size(), std::size_t{0});
}

CF_TEST(topology_domain_members_are_resolved_sorted_and_unique) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture first = add_rack(engine, "rack-a", "boot-1");
  const RackFixture second = add_rack(engine, "rack-b", "boot-2");

  MutationRequest request = engine.base(MutationKind::PublishPlacementDomain);
  PlacementDomain domain;
  domain.id = *PlacementDomainId::parse("placement-1");
  domain.klass = PlacementDomainClass::NetworkTier;
  domain.racks = {second.rack, first.rack, second.rack, first.rack};
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = request.evidence;
  request.placement_domain = domain;
  CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);

  const ClusterState published = engine.state();
  const PlacementDomain& stored =
      published.placement_domains.at(*PlacementDomainId::parse("placement-1"));
  CF_EXPECT_EQ(stored.racks.size(), std::size_t{2});
  CF_EXPECT_EQ(stored.racks.at(0), first.rack);
  CF_EXPECT_EQ(stored.racks.at(1), second.rack);

  // A member that is not a cluster member is refused.
  MutationRequest unknown = request;
  unknown.placement_domain->header.generation = DomainGeneration::from_raw(2);
  unknown.placement_domain->racks = {first.rack, make_rack("rack-missing")};
  CF_EXPECT_EQ(engine.submit(unknown).reason, RejectionReason::UnknownRack);

  // The derived index matches the canonical record.
  ClusterIndexes indexes;
  indexes.rebuild(engine.state());
  const std::vector<RackId>& members =
      indexes.racks_in_placement_domain(*PlacementDomainId::parse("placement-1"));
  CF_EXPECT_EQ(members.size(), std::size_t{2});
  CF_EXPECT_EQ(members.at(0), first.rack);
  CF_EXPECT_EQ(members.at(1), second.rack);
}

CF_TEST(topology_power_and_cooling_parents_are_recorded) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "boot-1");

  MutationRequest root = engine.base(MutationKind::PublishPowerDomain);
  PowerDomain root_domain;
  root_domain.id = *PowerDomainId::parse("power-root");
  root_domain.racks = {rack.rack};
  root_domain.header.generation = DomainGeneration::from_raw(1);
  root_domain.header.evidence = root.evidence;
  root.power_domain = root_domain;
  CF_EXPECT_EQ(engine.submit(root).outcome, MutationOutcome::Accepted);
  CF_EXPECT(!engine.state().power_domains.begin()->second.parent.has_value());

  MutationRequest child = engine.base(MutationKind::PublishPowerDomain);
  PowerDomain child_domain;
  child_domain.id = *PowerDomainId::parse("power-child");
  child_domain.racks = {rack.rack};
  child_domain.parent = root_domain.id;
  child_domain.header.generation = DomainGeneration::from_raw(1);
  child_domain.header.evidence = child.evidence;
  child.power_domain = child_domain;
  CF_EXPECT_EQ(engine.submit(child).outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  const PowerDomain& stored_child =
      published.power_domains.at(*PowerDomainId::parse("power-child"));
  CF_EXPECT_EQ(stored_child.parent, root_domain.id);

  MutationRequest cooling = engine.base(MutationKind::PublishCoolingDomain);
  CoolingDomain cooling_domain;
  cooling_domain.id = *CoolingDomainId::parse("cooling-child");
  cooling_domain.racks = {rack.rack};
  cooling_domain.parent = *CoolingDomainId::parse("cooling-root");
  cooling_domain.header.generation = DomainGeneration::from_raw(1);
  cooling_domain.header.evidence = cooling.evidence;
  cooling.cooling_domain = cooling_domain;
  CF_EXPECT_EQ(engine.submit(cooling).outcome, MutationOutcome::Accepted);
  CF_EXPECT_EQ(engine.state().cooling_domains.begin()->second.parent, cooling_domain.parent);
  CF_EXPECT(engine.coordinator().invariants().ok());
}

// ---------------------------------------------------------------------------
// Capacity quantities
// ---------------------------------------------------------------------------

CF_TEST(topology_capacity_quantities_are_unit_keyed_and_sorted) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "boot-1");

  MutationRequest request = engine.base(MutationKind::PublishCapacityDomain);
  CapacityDomain domain;
  domain.id = *CapacityDomainId::parse("capacity-1");
  domain.klass = CapacityDomainClass::RackGroup;
  domain.racks = {rack.rack};
  domain.quantities = {quantity("zeta", 3.0, EvidenceProvenance::Measured),
                       quantity("alpha", 1.0, EvidenceProvenance::Measured),
                       quantity("mid", std::nullopt, EvidenceProvenance::Reported)};
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = request.evidence;
  request.capacity_domain = domain;
  CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);

  const ClusterState published = engine.state();
  const CapacityDomain& stored =
      published.capacity_domains.at(*CapacityDomainId::parse("capacity-1"));
  CF_EXPECT_EQ(stored.quantities.size(), std::size_t{3});
  CF_EXPECT_EQ(stored.quantities.at(0).unit, std::string("alpha"));
  CF_EXPECT_EQ(stored.quantities.at(1).unit, std::string("mid"));
  CF_EXPECT_EQ(stored.quantities.at(2).unit, std::string("zeta"));
  // An absent value stays absent: UNKNOWN is never coerced to zero.
  CF_EXPECT(!stored.quantities.at(1).value.has_value());
  CF_EXPECT_EQ(stored.quantities.at(0).value.value(), 1.0);

  const CapacityAggregate aggregate =
      aggregate_capacity(engine.state(), *CapacityDomainId::parse("capacity-1"), "alpha");
  CF_EXPECT_EQ(aggregate.status, CapacityAggregateStatus::Known);
  CF_EXPECT(aggregate.value.has_value());
  CF_EXPECT_EQ(aggregate.value.value(), 1.0);
  CF_EXPECT_EQ(aggregate.provenance, EvidenceProvenance::Measured);

  // The same unit twice is refused: quantities with the same unit but
  // different semantics are never silently combined.
  MutationRequest duplicate = request;
  duplicate.capacity_domain->header.generation = DomainGeneration::from_raw(2);
  duplicate.capacity_domain->quantities.push_back(
      quantity("alpha", 9.0, EvidenceProvenance::Reported));
  const MutationResult conflict = engine.submit(duplicate);
  CF_EXPECT_EQ(conflict.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(conflict.reason, RejectionReason::Conflict);

  // A quantity without a unit is refused.
  MutationRequest no_unit = request;
  no_unit.capacity_domain->header.generation = DomainGeneration::from_raw(3);
  no_unit.capacity_domain->quantities = {quantity("", 1.0, EvidenceProvenance::Measured)};
  CF_EXPECT_EQ(engine.submit(no_unit).reason, RejectionReason::InvalidDomain);

  // A unit that is not a bounded label is refused.
  MutationRequest bad_unit = request;
  bad_unit.capacity_domain->header.generation = DomainGeneration::from_raw(4);
  bad_unit.capacity_domain->quantities = {quantity("bad\nunit", 1.0, EvidenceProvenance::Measured)};
  CF_EXPECT_EQ(engine.submit(bad_unit).reason, RejectionReason::InvalidDomain);

  // The quantity count is bounded.
  MutationRequest too_many = request;
  too_many.capacity_domain->header.generation = DomainGeneration::from_raw(5);
  too_many.capacity_domain->quantities.clear();
  for (std::size_t index = 0; index <= kMaxCapacityQuantities; ++index) {
    too_many.capacity_domain->quantities.push_back(
        quantity("unit-" + std::to_string(index), 1.0, EvidenceProvenance::Measured));
  }
  CF_EXPECT_EQ(engine.submit(too_many).reason, RejectionReason::LimitExceeded);
  CF_EXPECT_EQ(engine.state().capacity_domains.size(), std::size_t{1});
}

CF_TEST(topology_capacity_rejects_non_finite_values) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "boot-1");

  const auto publish_value = [&engine, &rack](std::uint64_t generation, double value) {
    MutationRequest request = engine.base(MutationKind::PublishCapacityDomain);
    CapacityDomain domain;
    domain.id = *CapacityDomainId::parse("capacity-1");
    domain.klass = CapacityDomainClass::CpuPool;
    domain.racks = {rack.rack};
    domain.quantities = {quantity("cores", value, EvidenceProvenance::Measured)};
    domain.header.generation = DomainGeneration::from_raw(generation);
    domain.header.evidence = request.evidence;
    request.capacity_domain = domain;
    return engine.submit(request);
  };

  const MutationResult nan = publish_value(1, std::numeric_limits<double>::quiet_NaN());
  CF_EXPECT_EQ(nan.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(nan.reason, RejectionReason::InvalidDomain);

  const MutationResult infinity = publish_value(2, std::numeric_limits<double>::infinity());
  if (infinity.outcome != MutationOutcome::Rejected ||
      infinity.reason != RejectionReason::InvalidDomain) {
    CF_FAIL("a capacity quantity with an infinite value was not refused: outcome " +
            std::string(to_string(infinity.outcome)) + " reason " +
            std::string(to_string(infinity.reason)));
  }
  CF_EXPECT_EQ(engine.state().capacity_domains.size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Constraints
// ---------------------------------------------------------------------------

CF_TEST(topology_constraint_publishing_rules) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture first = add_rack(engine, "rack-a", "boot-1");
  const RackFixture second = add_rack(engine, "rack-b", "boot-2");

  MutationRequest request = engine.base(MutationKind::PublishConstraint);
  ClusterConstraint constraint;
  constraint.id = *ConstraintId::parse("constraint-1");
  constraint.kind = ConstraintKind::FailureDomainIndependence;
  constraint.racks = {second.rack, first.rack, second.rack};
  constraint.statement = "two racks must not share a power feed";
  constraint.header.generation = DomainGeneration::from_raw(1);
  constraint.header.evidence = request.evidence;
  request.constraint = constraint;
  const MutationResult accepted = engine.submit(request);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);
  const ClusterState published = engine.state();
  CF_EXPECT_EQ(published.constraints.size(), std::size_t{1});
  CF_EXPECT_EQ(published.constraints.begin()->second.racks.size(), std::size_t{2});
  CF_EXPECT_EQ(published.constraints.begin()->second.racks.at(0), first.rack);
  CF_EXPECT_EQ(published.constraint_generation, ConstraintGeneration::from_raw(2));
  CF_EXPECT_EQ(published.constraints.begin()->second.header.cluster_epoch, published.epoch);

  // Identical repeat.
  MutationRequest repeat = request;
  repeat.constraint = published.constraints.begin()->second;
  CF_EXPECT_EQ(engine.submit(repeat).outcome, MutationOutcome::NoChange);

  // UNKNOWN kind.
  MutationRequest unknown_kind = request;
  unknown_kind.constraint->kind = ConstraintKind::Unknown;
  unknown_kind.constraint->header.generation = DomainGeneration::from_raw(2);
  CF_EXPECT_EQ(engine.submit(unknown_kind).reason, RejectionReason::InvalidDomain);

  // Members must resolve.
  MutationRequest unknown_member = request;
  unknown_member.constraint->header.generation = DomainGeneration::from_raw(3);
  unknown_member.constraint->racks = {make_rack("rack-missing")};
  CF_EXPECT_EQ(engine.submit(unknown_member).reason, RejectionReason::UnknownRack);

  // A zero generation is refused.
  MutationRequest zero_generation = request;
  zero_generation.constraint->header.generation = DomainGeneration{};
  CF_EXPECT_EQ(engine.submit(zero_generation).reason, RejectionReason::InvalidDomain);

  // A conflicting publication at the same generation.
  MutationRequest conflicting = request;
  conflicting.constraint->statement = "a different statement";
  const MutationResult conflict = engine.submit(conflicting);
  CF_EXPECT_EQ(conflict.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(conflict.reason, RejectionReason::Conflict);
  CF_EXPECT_EQ(conflict.error.reason, std::string("REJECT_CONFLICT"));

  // An older generation is stale. A newer generation must exist first for the
  // publication to be older than the authoritative one.
  MutationRequest superseding = request;
  superseding.constraint->header.generation = DomainGeneration::from_raw(2);
  superseding.constraint->statement = "a superseding statement";
  CF_EXPECT_EQ(engine.submit(superseding).outcome, MutationOutcome::Accepted);
  MutationRequest stale = request;
  stale.constraint->header.generation = DomainGeneration::from_raw(1);
  stale.constraint->statement = "an older statement";
  const MutationResult stale_result = engine.submit(stale);
  CF_EXPECT_EQ(stale_result.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(stale_result.reason, RejectionReason::StalePublication);

  MutationRequest older = request;
  older.constraint->header.generation = DomainGeneration{};
  CF_EXPECT_EQ(engine.submit(older).reason, RejectionReason::InvalidDomain);

  // A newer generation supersedes the record.
  MutationRequest newer = request;
  newer.constraint->header.generation = DomainGeneration::from_raw(7);
  newer.constraint->statement = "superseding statement";
  CF_EXPECT_EQ(engine.submit(newer).outcome, MutationOutcome::Accepted);
  CF_EXPECT_EQ(engine.state().constraints.begin()->second.header.generation,
               DomainGeneration::from_raw(7));
  CF_EXPECT_EQ(engine.state().constraint_generation, ConstraintGeneration::from_raw(4));
}

CF_TEST(topology_constraint_stale_generation_is_rejected) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture rack = add_rack(engine, "rack-a", "boot-1");

  MutationRequest request = engine.base(MutationKind::PublishConstraint);
  ClusterConstraint constraint;
  constraint.id = *ConstraintId::parse("constraint-1");
  constraint.kind = ConstraintKind::CapacityFloor;
  constraint.racks = {rack.rack};
  constraint.statement = "at least eight devices";
  constraint.header.generation = DomainGeneration::from_raw(5);
  constraint.header.evidence = request.evidence;
  request.constraint = constraint;
  CF_EXPECT_EQ(engine.submit(request).outcome, MutationOutcome::Accepted);

  MutationRequest stale = request;
  stale.constraint->header.generation = DomainGeneration::from_raw(4);
  stale.constraint->statement = "an older statement";
  const MutationResult result = engine.submit(stale);
  CF_EXPECT_EQ(result.outcome, MutationOutcome::Rejected);
  CF_EXPECT_EQ(result.reason, RejectionReason::StalePublication);
  CF_EXPECT_EQ(result.error.reason, std::string("stale_constraint_generation"));
  CF_EXPECT_EQ(engine.state().constraints.begin()->second.header.generation,
               DomainGeneration::from_raw(5));
}

// ---------------------------------------------------------------------------
// Topology supersession
// ---------------------------------------------------------------------------

CF_TEST(topology_supersede_advances_epoch_and_invalidates_links) {
  Engine engine;
  declare_cluster(engine);
  const RackFixture first = add_rack(engine, "rack-a", "boot-1");
  const RackFixture second = add_rack(engine, "rack-b", "boot-2");
  const TopologyEpoch epoch = engine.state().topology_epoch;

  CF_EXPECT_EQ(publish_link(engine, make_link("link-a-b", first.rack, second.rack, epoch,
                                              engine.evidence(), 1))
                   .outcome,
               MutationOutcome::Accepted);
  CF_EXPECT_EQ(publish_link(engine, make_link("link-b-a", second.rack, first.rack, epoch,
                                              engine.evidence(), 1))
                   .outcome,
               MutationOutcome::Accepted);
  const ClusterState before = engine.state();
  CF_EXPECT_EQ(before.links.size(), std::size_t{2});

  MutationRequest supersede = engine.base(MutationKind::SupersedeTopology);
  supersede.target_topology_epoch = *epoch.next();
  supersede.topology_reason = "fabric_rewired";
  const MutationResult accepted = engine.submit(supersede);
  CF_EXPECT_EQ(accepted.outcome, MutationOutcome::Accepted);

  const ClusterState after = engine.state();
  CF_EXPECT_EQ(after.topology_epoch, *epoch.next());
  CF_EXPECT_EQ(after.topology_generation, *before.topology_generation.next());
  CF_EXPECT_EQ(after.connectivity_generation, *before.connectivity_generation.next());
  CF_EXPECT_EQ(after.topology_record.epoch, after.topology_epoch);
  CF_EXPECT_EQ(after.topology_record.generation, after.topology_generation);
  CF_EXPECT_EQ(after.topology_record.reason, std::string("fabric_rewired"));
  CF_EXPECT_EQ(after.topology_record.established_at, engine.now());
  CF_EXPECT_EQ(after.topology_record.evidence.provenance, EvidenceProvenance::Reported);
  CF_EXPECT_EQ(after.links.size(), std::size_t{2});
  for (const auto& entry : after.links) {
    CF_EXPECT(entry.second.header.evidence.requires_revalidation());
    CF_EXPECT_EQ(entry.second.reachability, Reachability::Unknown);
    CF_EXPECT_EQ(entry.second.health, HealthState::Unknown);
  }
  // A link whose evidence requires revalidation is not a current statement,
  // so reachability is reported as REVALIDATION_REQUIRED, not UNKNOWN.
  CF_EXPECT_EQ(reachability_between(after, first.rack, second.rack),
               Reachability::RevalidationRequired);

  // The superseded epoch and an older one are both refused.
  MutationRequest same = supersede;
  CF_EXPECT_EQ(engine.submit(same).reason, RejectionReason::StaleTopologyEpoch);
  MutationRequest older = supersede;
  older.target_topology_epoch = epoch;
  CF_EXPECT_EQ(engine.submit(older).reason, RejectionReason::StaleTopologyEpoch);

  // A link published under the superseded epoch is refused, under the new one
  // it is accepted.
  CF_EXPECT_EQ(publish_link(engine, make_link("link-c", first.rack, second.rack, epoch,
                                              engine.evidence(), 1))
                   .reason,
               RejectionReason::StaleTopologyEpoch);
  CF_EXPECT_EQ(publish_link(engine, make_link("link-c", first.rack, second.rack,
                                              after.topology_epoch, engine.evidence(), 1))
                   .outcome,
               MutationOutcome::Accepted);
  CF_EXPECT(engine.coordinator().invariants().ok());
}

int main() { return cf_test::run("test_topology_domains"); }
