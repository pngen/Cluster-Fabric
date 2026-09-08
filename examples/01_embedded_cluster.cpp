// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Example 1: an embedded cluster.
//
// Starts a real coordinator in this process, builds a synthetic eight-rack
// cluster through the public mutation API, then reads the result back through
// the snapshot contract. Every rack in this example is SYNTHETIC.

#include <iostream>
#include <string>

#include "cluster_fabric/cluster_fabric.hpp"

int main() {
  using namespace cluster_fabric;

  const ClusterId cluster = *ClusterId::parse("example-embedded-cluster");
  const RackPublisherId publisher = *RackPublisherId::parse("example-publisher");
  const RackAgentBootId boot = make_rack_agent_boot_id("example");

  SyntheticConfig synthetic;
  synthetic.seed = 20260101;
  synthetic.cluster = cluster;
  synthetic.publisher = publisher;
  synthetic.boot = boot;
  synthetic.rack_count = 8;
  synthetic.racks_per_placement_domain = 2;
  synthetic.racks_per_failure_domain = 2;
  synthetic.network_domain_count = 2;
  synthetic.link_density = 0.35;
  synthetic.heterogeneous = true;

  const SyntheticCluster scenario = generate_synthetic_cluster(synthetic);
  std::cout << "scenario: " << scenario.reproduction_parameters() << "\n";

  CoordinatorConfig config;
  config.cluster = cluster;
  config.port = 0;
  config.persist_on_commit = false;
  config.readiness_contract = ReadinessContract::permissive();
  config.readiness_contract.minimum_current_racks = 8;

  ClusterCoordinator coordinator(config);
  const CoordinatorStartOutcome started = coordinator.start();
  if (!started.ok) {
    std::cerr << "coordinator failed to start\n";
    return 1;
  }

  std::size_t accepted = 0;
  std::size_t refused = 0;
  for (const MutationRequest& request : scenario.requests) {
    const MutationResult result = coordinator.submit(request);
    if (result.accepted()) {
      ++accepted;
    } else {
      ++refused;
      std::cerr << "refused " << to_string(request.kind) << ": " << to_string(result.reason) << " "
                << result.error.message << "\n";
    }
  }
  std::cout << "mutations: accepted " << accepted << " refused " << refused << "\n";

  const SnapshotValidation synthetic_validation =
      coordinator.snapshot().validate(coordinator.state_copy());
  std::cout << "synthetic bound  : current=" << (synthetic_validation.current ? "yes" : "no")
            << " consumable=" << (synthetic_validation.consumable ? "yes" : "no")
            << " (generations match, but fabricated evidence never satisfies a current-evidence "
               "requirement)\n";

  // Re-publishing the same racks with evidence an authority actually reports is
  // the only thing that turns the composition current.
  std::size_t refreshed = 0;
  for (const RackId& rack : scenario.racks) {
    const ClusterState current = coordinator.state_copy();
    const auto it = current.racks.find(rack);
    if (it == current.racks.end()) {
      continue;
    }
    const RackRecord& record = it->second;
    MutationRequest request;
    request.kind = MutationKind::UpdateRackGeneration;
    request.cluster = cluster;
    request.authority.cluster_epoch = current.epoch;
    request.authority.coordinator_epoch = current.coordinator_epoch;
    request.authority.topology_epoch = current.topology_epoch;
    request.authority.publisher = record.reference.publisher;
    request.authority.publication =
        PublicationGeneration::from_raw(record.reference.publication.value() + 1);
    request.authority.boot = record.reference.boot;
    request.authority.rack = rack;
    request.expected_rack_generation = record.reference.generation;
    request.new_rack_generation = record.reference.generation.next().value();
    request.rack_reference = record.reference;
    request.rack_reference.generation = *request.new_rack_generation;
    request.rack_reference.evidence =
        EvidenceStamp::make(EvidenceProvenance::Reported, default_clock().now(), 60'000);
    request.evidence = request.rack_reference.evidence;
    request.reason = "example_republish_reported_evidence";
    if (coordinator.submit(request).accepted()) {
      ++refreshed;
    }
  }
  std::cout << "republished      : " << refreshed << " racks with REPORTED evidence\n";

  const ClusterSnapshot snapshot = coordinator.snapshot();
  const SnapshotValidation validation = snapshot.validate(coordinator.state_copy());
  std::cout << "snapshot digest  : " << snapshot.semantic_digest() << "\n";
  std::cout << "snapshot current : " << (validation.current ? "yes" : "no") << "\n";
  std::cout << "snapshot usable  : " << (validation.consumable ? "yes" : "no") << "\n";

  const ReadinessEvaluation readiness = coordinator.readiness();
  std::cout << "lifecycle        : " << to_string(readiness.lifecycle) << "\n";
  std::cout << "contract         : " << (readiness.satisfied ? "satisfied" : "unsatisfied")
            << "\n";
  for (const ReadinessBlocker& blocker : readiness.blockers) {
    std::cout << "blocker          : " << blocker.code << " " << blocker.subject << " "
              << blocker.detail << "\n";
  }

  const InvariantReport invariants = coordinator.invariants();
  std::cout << "invariants       : " << (invariants.ok() ? "ok" : invariants.describe()) << "\n";
  std::cout << coordinator.explain_lifecycle().describe() << "\n";

  coordinator.stop();
  return invariants.ok() ? 0 : 1;
}
