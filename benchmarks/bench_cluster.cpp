// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Benchmark: completed mutating operations.
//
// Every reported number is derived from operations that actually completed and
// were committed. Nothing is extrapolated from a partial run, and no timer
// terminates the run: the workload size is fixed by the command line.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "cluster_fabric/cluster_fabric.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
  std::string label;
  std::size_t completed = 0;
  double millis = 0.0;

  [[nodiscard]] double per_second() const {
    if (millis <= 0.0) {
      return 0.0;
    }
    return (static_cast<double>(completed) * 1000.0) / millis;
  }
};

class Reporter {
 public:
  void add(const Measurement& measurement) {
    measurements_.push_back(measurement);
    std::cout << measurement.label << ": " << measurement.completed << " operations in "
              << measurement.millis << " ms -> " << measurement.per_second() << " ops/s\n";
  }

  [[nodiscard]] bool all_completed() const {
    for (const Measurement& measurement : measurements_) {
      if (measurement.completed == 0) {
        return false;
      }
    }
    return true;
  }

 private:
  std::vector<Measurement> measurements_;
};

cluster_fabric::RackId rack_id(std::size_t index) {
  return *cluster_fabric::RackId::parse("bench-rack-" + std::to_string(index));
}

cluster_fabric::MutationRequest base_request(const cluster_fabric::ClusterId& cluster,
                                             const cluster_fabric::CoordinatorEpoch& epoch,
                                             const cluster_fabric::RackPublisherId& publisher) {
  cluster_fabric::MutationRequest request;
  request.kind = cluster_fabric::MutationKind::AddRack;
  request.cluster = cluster;
  request.authority.cluster_epoch = cluster_fabric::ClusterEpoch::from_raw(1);
  request.authority.coordinator_epoch = epoch;
  request.authority.topology_epoch = cluster_fabric::TopologyEpoch::from_raw(1);
  request.authority.publisher = publisher;
  request.authority.publication = cluster_fabric::PublicationGeneration::from_raw(1);
  request.evidence = cluster_fabric::EvidenceStamp::make(
      cluster_fabric::EvidenceProvenance::Synthetic, cluster_fabric::default_clock().now(), 30'000);
  return request;
}

void run(std::size_t rack_count, std::size_t link_target, Reporter& reporter) {
  using namespace cluster_fabric;

  const ClusterId cluster = *ClusterId::parse("bench-cluster-" + std::to_string(rack_count));
  const RackPublisherId publisher = *RackPublisherId::parse("bench-publisher");

  CoordinatorConfig config;
  config.cluster = cluster;
  config.port = 0;
  config.persist_on_commit = false;
  config.max_racks = kMaxRacksPerCluster;
  config.max_links = kMaxInterRackLinks;

  ClusterCoordinator coordinator(config);
  if (!coordinator.start().ok) {
    std::cerr << "coordinator failed to start\n";
    return;
  }
  const CoordinatorEpoch epoch = coordinator.recovery().current_coordinator_epoch;

  MutationRequest declare;
  declare.kind = MutationKind::DeclareCluster;
  declare.cluster = cluster;
  declare.authority.cluster_epoch = ClusterEpoch::from_raw(1);
  declare.authority.coordinator_epoch = epoch;
  declare.readiness_contract = ReadinessContract::permissive();
  declare.evidence = EvidenceStamp::make(EvidenceProvenance::Synthetic, default_clock().now(), 0);
  (void)coordinator.submit(declare);

  std::cout << "--- " << rack_count << " racks, link target " << link_target << " ---\n";

  {
    const Clock::time_point start = Clock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < rack_count; ++i) {
      MutationRequest request = base_request(cluster, epoch, publisher);
      request.rack_reference.rack = rack_id(i);
      request.rack_reference.generation = RackGeneration::from_raw(1);
      request.rack_reference.publisher = publisher;
      request.rack_reference.rack_lifecycle = RackLifecycleState::Ready;
      request.rack_reference.evidence = request.evidence;
      AcceleratorClassSummary summary;
      summary.vendor = AcceleratorVendor::Nvidia;
      summary.family = "synthetic";
      summary.device_count = 8u;
      summary.device_memory_bytes = 32ull * 1024 * 1024 * 1024;
      request.rack_reference.composition.accelerators.push_back(summary);
      request.rack_reference.composition.provenance = EvidenceProvenance::Synthetic;
      if (coordinator.submit(request).accepted()) {
        ++completed;
      }
    }
    const Clock::time_point end = Clock::now();
    reporter.add({"add_rack",
                  completed,
                  std::chrono::duration<double, std::milli>(end - start).count()});
  }

  {
    const Clock::time_point start = Clock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < link_target; ++i) {
      const std::size_t source = i % rack_count;
      const std::size_t destination = (i * 7 + 1) % rack_count;
      if (source == destination) {
        continue;
      }
      MutationRequest request = base_request(cluster, epoch, publisher);
      request.kind = MutationKind::PublishInterRackLink;
      request.link = InterRackLink{};
      request.link->id = synthetic_link_id(rack_id(source), rack_id(destination));
      request.link->source = rack_id(source);
      request.link->destination = rack_id(destination);
      request.link->direction = LinkDirection::Bidirectional;
      request.link->connectivity = ConnectivityClass::SwitchedFabric;
      request.link->reachability = Reachability::Reachable;
      request.link->health = HealthState::Healthy;
      request.link->topology_epoch = TopologyEpoch::from_raw(1);
      request.link->header.generation = DomainGeneration::from_raw(static_cast<std::uint64_t>(i) + 1);
      request.link->header.evidence = request.evidence;
      request.link->bandwidth_class = BandwidthClass::High;
      request.link->latency_class = LatencyClass::Low;
      if (coordinator.submit(request).accepted()) {
        ++completed;
      }
    }
    const Clock::time_point end = Clock::now();
    reporter.add({"publish_link",
                  completed,
                  std::chrono::duration<double, std::milli>(end - start).count()});
  }

  {
    const Clock::time_point start = Clock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < 64; ++i) {
      MutationRequest request = base_request(cluster, epoch, publisher);
      request.kind = MutationKind::PublishPlacementDomain;
      request.placement_domain = PlacementDomain{};
      request.placement_domain->id =
          *PlacementDomainId::parse("bench-pd-" + std::to_string(i));
      request.placement_domain->klass = PlacementDomainClass::LowLatencyFabric;
      for (std::size_t r = 0; r < rack_count; ++r) {
        request.placement_domain->racks.push_back(rack_id(r));
      }
      request.placement_domain->header.generation = DomainGeneration::from_raw(1);
      request.placement_domain->header.evidence = request.evidence;
      if (coordinator.submit(request).accepted()) {
        ++completed;
      }
    }
    const Clock::time_point end = Clock::now();
    reporter.add({"publish_placement_domain",
                  completed,
                  std::chrono::duration<double, std::milli>(end - start).count()});
  }

  {
    const Clock::time_point start = Clock::now();
    std::size_t completed = 0;
    const std::size_t iterations = 64;
    for (std::size_t i = 0; i < iterations; ++i) {
      const ClusterSnapshot snapshot = coordinator.snapshot();
      if (!snapshot.semantic_digest().empty()) {
        ++completed;
      }
    }
    const Clock::time_point end = Clock::now();
    reporter.add({"capture_snapshot",
                  completed,
                  std::chrono::duration<double, std::milli>(end - start).count()});
  }

  {
    const ClusterSnapshot snapshot = coordinator.snapshot();
    const Clock::time_point start = Clock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < 64; ++i) {
      const SnapshotValidation validation = coordinator.validate(snapshot);
      if (validation.current) {
        ++completed;
      }
    }
    const Clock::time_point end = Clock::now();
    reporter.add({"validate_snapshot",
                  completed,
                  std::chrono::duration<double, std::milli>(end - start).count()});
  }

  {
    const Clock::time_point start = Clock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < 256; ++i) {
      const ReadinessEvaluation evaluation = coordinator.readiness();
      if (evaluation.lifecycle != ClusterLifecycle::Declared) {
        ++completed;
      }
    }
    const Clock::time_point end = Clock::now();
    reporter.add({"evaluate_readiness",
                  completed,
                  std::chrono::duration<double, std::milli>(end - start).count()});
  }

  {
    const Clock::time_point start = Clock::now();
    std::size_t completed = 0;
    for (std::size_t i = 0; i < 256; ++i) {
      if (coordinator.invariants().ok()) {
        ++completed;
      }
    }
    const Clock::time_point end = Clock::now();
    reporter.add({"check_invariants",
                  completed,
                  std::chrono::duration<double, std::milli>(end - start).count()});
  }

  const CoordinatorStats stats = coordinator.stats();
  std::cout << "commits " << stats.commits << " accepted " << stats.mutations_accepted
            << " rejected " << stats.mutations_rejected << "\n";
  coordinator.stop();
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t max_racks = 1000;
  std::size_t max_links = 100000;
  if (argc > 1) {
    max_racks = static_cast<std::size_t>(std::stoull(argv[1]));
  }
  if (argc > 2) {
    max_links = static_cast<std::size_t>(std::stoull(argv[2]));
  }

  Reporter reporter;
  for (const std::size_t racks : {std::size_t{10}, std::size_t{100}, std::size_t{1000}}) {
    if (racks > max_racks) {
      continue;
    }
    const std::size_t links = (std::min)(max_links, racks * 100);
    run(racks, links, reporter);
  }
  std::cout << (reporter.all_completed() ? "all measurements completed\n"
                                         : "incomplete measurements detected\n");
  return reporter.all_completed() ? 0 : 1;
}
