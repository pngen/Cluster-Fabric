// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Example 2: the consumer contract.
//
// A higher-level runtime must never act on a cluster view it cannot prove
// current. This example shows the exact contract: capture a snapshot, keep the
// binding, and re-validate before use. A superseded snapshot is refused with
// typed reasons instead of silently consumed.

#include <iostream>
#include <string>

#include "cluster_fabric/cluster_fabric.hpp"

int main() {
  using namespace cluster_fabric;

  const ClusterId cluster = *ClusterId::parse("example-consumer-cluster");
  const RackPublisherId publisher = *RackPublisherId::parse("example-publisher");

  CoordinatorConfig config;
  config.cluster = cluster;
  config.port = 0;
  config.persist_on_commit = false;

  ClusterCoordinator coordinator(config);
  if (!coordinator.start().ok) {
    std::cerr << "coordinator failed to start\n";
    return 1;
  }

  MutationRequest declare;
  declare.kind = MutationKind::DeclareCluster;
  declare.cluster = cluster;
  declare.authority.cluster_epoch = ClusterEpoch::from_raw(1);
  declare.authority.coordinator_epoch = coordinator.recovery().current_coordinator_epoch;
  declare.readiness_contract = ReadinessContract::permissive();
  declare.evidence = EvidenceStamp::make(EvidenceProvenance::Reported, default_clock().now(), 0);
  (void)coordinator.submit(declare);

  MutationRequest add;
  add.kind = MutationKind::AddRack;
  add.cluster = cluster;
  add.authority.cluster_epoch = ClusterEpoch::from_raw(1);
  add.authority.coordinator_epoch = coordinator.recovery().current_coordinator_epoch;
  add.authority.topology_epoch = TopologyEpoch::from_raw(1);
  add.authority.publisher = publisher;
  add.authority.publication = PublicationGeneration::from_raw(1);
  add.rack_reference.rack = *RackId::parse("rack-example-0");
  add.rack_reference.generation = RackGeneration::from_raw(1);
  add.rack_reference.publisher = publisher;
  add.rack_reference.rack_lifecycle = RackLifecycleState::Ready;
  add.rack_reference.evidence =
      EvidenceStamp::make(EvidenceProvenance::Synthetic, default_clock().now(), 30'000);
  add.evidence = add.rack_reference.evidence;
  AcceleratorClassSummary summary;
  summary.vendor = AcceleratorVendor::Nvidia;
  summary.family = "synthetic";
  summary.device_count = 8u;
  summary.device_memory_bytes = 32ull * 1024 * 1024 * 1024;
  add.rack_reference.composition.accelerators.push_back(summary);
  add.rack_reference.composition.provenance = EvidenceProvenance::Synthetic;
  const MutationResult added = coordinator.submit(add);
  std::cout << "add rack: " << to_string(added.outcome) << " " << to_string(added.reason);
  if (!added.accepted()) {
    std::cout << " (" << added.error.reason << ": " << added.error.message << ")";
  }
  std::cout << "\n";

  const ClusterSnapshot snapshot = coordinator.snapshot();
  std::cout << "captured generation " << snapshot.cluster_generation().str() << " digest "
            << snapshot.semantic_digest() << "\n";

  SnapshotValidation before = snapshot.validate(coordinator.state_copy());
  std::cout << "immediately usable : " << (before.consumable ? "yes" : "no") << "\n";

  // The cluster changes underneath the consumer.
  MutationRequest advance = add;
  advance.kind = MutationKind::UpdateRackGeneration;
  advance.authority.publication = PublicationGeneration::from_raw(2);
  advance.expected_rack_generation = RackGeneration::from_raw(1);
  advance.new_rack_generation = RackGeneration::from_raw(2);
  advance.rack_reference.generation = RackGeneration::from_raw(2);
  advance.evidence = EvidenceStamp::make(EvidenceProvenance::Synthetic, default_clock().now(), 30'000);
  const MutationResult advanced = coordinator.submit(advance);
  std::cout << "generation advance: " << to_string(advanced.outcome) << "\n";

  SnapshotValidation after = snapshot.validate(coordinator.state_copy());
  std::cout << "still usable       : " << (after.consumable ? "yes" : "no") << "\n";
  for (std::size_t i = 0; i < after.reasons.size(); ++i) {
    std::cout << "typed stale reason : " << to_string(after.reasons[i]);
    if (i < after.subjects.size()) {
      std::cout << " " << after.subjects[i];
    }
    std::cout << "\n";
  }
  std::cout << after.explanation.describe() << "\n";

  coordinator.stop();
  return after.consumable ? 1 : 0;
}
