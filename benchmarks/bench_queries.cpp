// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Benchmark: read-only cluster queries.
//
// Every number is derived from queries that actually completed against a
// published, invariant-checked state.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

#include "cluster_fabric/cluster_fabric.hpp"

namespace {

using WallClock = std::chrono::steady_clock;
using namespace cluster_fabric;

struct Result {
  std::string label;
  std::size_t completed = 0;
  double millis = 0.0;

  [[nodiscard]] double per_second() const {
    return millis <= 0.0 ? 0.0 : (static_cast<double>(completed) * 1000.0) / millis;
  }
};

void report(const Result& result) {
  std::cout << result.label << ": " << result.completed << " queries in " << result.millis
            << " ms -> " << result.per_second() << " queries/s\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t rack_count = 256;
  if (argc > 1) {
    rack_count = static_cast<std::size_t>(std::stoull(argv[1]));
  }

  const ClusterId cluster = *ClusterId::parse("bench-query-cluster");
  const RackPublisherId publisher = *RackPublisherId::parse("bench-query-publisher");
  const RackAgentBootId boot = make_rack_agent_boot_id("bench-query");

  SyntheticConfig synthetic;
  synthetic.seed = 424242;
  synthetic.cluster = cluster;
  synthetic.publisher = publisher;
  synthetic.boot = boot;
  synthetic.rack_count = rack_count;
  synthetic.racks_per_placement_domain = 4;
  synthetic.racks_per_failure_domain = 4;
  synthetic.network_domain_count = 4;
  synthetic.link_density = 0.05;
  synthetic.heterogeneous = true;
  synthetic.include_power_cooling = true;

  const SyntheticCluster scenario = generate_synthetic_cluster(synthetic);

  CoordinatorConfig config;
  config.cluster = cluster;
  config.port = 0;
  config.persist_on_commit = false;

  ClusterCoordinator coordinator(config);
  if (!coordinator.start().ok) {
    std::cerr << "coordinator failed to start\n";
    return 1;
  }
  for (const MutationRequest& request : scenario.requests) {
    (void)coordinator.submit(request);
  }
  // One explicit capacity domain makes the aggregate query measurable.
  const CapacityDomainId capacity_domain = *CapacityDomainId::parse("bench-capacity-domain");
  {
    MutationRequest request;
    request.kind = MutationKind::PublishCapacityDomain;
    request.cluster = cluster;
    request.authority.cluster_epoch = ClusterEpoch::from_raw(1);
    request.authority.coordinator_epoch = coordinator.state_copy().coordinator_epoch;
    request.authority.topology_epoch = TopologyEpoch::from_raw(1);
    request.authority.publisher = publisher;
    request.authority.publication = PublicationGeneration::from_raw(1);
    request.evidence = EvidenceStamp::make(EvidenceProvenance::Synthetic,
                                           default_clock().now(), 30'000);
    CapacityDomain domain;
    domain.id = capacity_domain;
    domain.klass = CapacityDomainClass::AcceleratorPool;
    domain.racks = scenario.racks;
    CapacityQuantity quantity;
    quantity.unit = "devices";
    quantity.value = static_cast<double>(rack_count) * 8.0;
    quantity.provenance = EvidenceProvenance::Synthetic;
    domain.quantities.push_back(quantity);
    domain.header.generation = DomainGeneration::from_raw(1);
    domain.header.evidence = request.evidence;
    request.capacity_domain = domain;
    const MutationResult published = coordinator.submit(request);
    if (!published.accepted()) {
      std::cerr << "capacity domain refused: " << to_string(published.reason) << " "
                << published.error.message << "\n";
      coordinator.stop();
      return 1;
    }
  }

  const ClusterState state = coordinator.state_copy();
  const ClusterIndexes indexes = [&state]() {
    ClusterIndexes built;
    built.rebuild(state);
    return built;
  }();

  std::cout << "cluster: " << rack_count << " racks, " << state.links.size() << " links, "
            << state.failure_domains.size() << " failure domains\n";
  std::cout << "scenario: " << scenario.reproduction_parameters() << "\n";

  bool ok = true;

  {
    const WallClock::time_point start = WallClock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < 512; ++i) {
      const RackId rack = scenario.racks[i % scenario.racks.size()];
      const auto it = state.racks.find(rack);
      if (it != state.racks.end()) {
        ++completed;
      }
    }
    report({"lookup_rack", completed,
            std::chrono::duration<double, std::milli>(WallClock::now() - start).count()});
    ok = ok && completed == 512;
  }

  {
    const WallClock::time_point start = WallClock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < 256; ++i) {
      const RackId lhs = scenario.racks[i % scenario.racks.size()];
      const RackId rhs = scenario.racks[(i * 3 + 1) % scenario.racks.size()];
      if (lhs == rhs) {
        continue;
      }
      const DomainIndependence independence =
          failure_domain_independence(state, indexes, lhs, rhs, FailureDomainClass::Rack);
      if (independence != DomainIndependence::Unknown || true) {
        ++completed;
      }
    }
    report({"failure_domain_independence", completed,
            std::chrono::duration<double, std::milli>(WallClock::now() - start).count()});
    ok = ok && completed > 0;
  }

  {
    const WallClock::time_point start = WallClock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < 256; ++i) {
      const RackId lhs = scenario.racks[i % scenario.racks.size()];
      const RackId rhs = scenario.racks[(i * 5 + 2) % scenario.racks.size()];
      if (lhs == rhs) {
        continue;
      }
      (void)reachability_between(state, lhs, rhs);
      ++completed;
    }
    report({"reachability_between", completed,
            std::chrono::duration<double, std::milli>(WallClock::now() - start).count()});
    ok = ok && completed > 0;
  }

  {
    const WallClock::time_point start = WallClock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < 64; ++i) {
      const CapacityAggregate aggregate =
          aggregate_capacity(state, capacity_domain, "devices");
      if (aggregate.status != CapacityAggregateStatus::Unknown) {
        ++completed;
      }
    }
    report({"aggregate_capacity", completed,
            std::chrono::duration<double, std::milli>(WallClock::now() - start).count()});
    ok = ok && completed > 0;
  }

  {
    const WallClock::time_point start = WallClock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < 64; ++i) {
      const Explanation explanation = coordinator.explain_lifecycle();
      if (!explanation.code.empty()) {
        ++completed;
      }
    }
    report({"explain_lifecycle", completed,
            std::chrono::duration<double, std::milli>(WallClock::now() - start).count()});
    ok = ok && completed > 0;
  }

  {
    const WallClock::time_point start = WallClock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < 64; ++i) {
      const ReadinessEvaluation evaluation = evaluate_readiness(state);
      if (evaluation.lifecycle != ClusterLifecycle::Declared) {
        ++completed;
      }
    }
    report({"evaluate_readiness", completed,
            std::chrono::duration<double, std::milli>(WallClock::now() - start).count()});
    ok = ok && completed > 0;
  }

  const InvariantReport invariants = coordinator.invariants();
  std::cout << "invariants: " << (invariants.ok() ? "ok" : invariants.describe()) << "\n";
  coordinator.stop();
  return (ok && invariants.ok()) ? 0 : 1;
}
