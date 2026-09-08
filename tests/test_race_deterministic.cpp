// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic concurrency: conflicting publications, concurrent membership
// changes, concurrent readers and repeatability.
//
// The properties under test: a same-generation conflict has exactly one
// winner, concurrent distinct membership changes produce exactly the state a
// sequential replay produces, a concurrent reader never observes a torn state
// and never sees a current snapshot whose digest disagrees with current state,
// and a deterministic scenario repeated in one process always produces the
// same semantic digest.

#include "harness.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

namespace {

constexpr std::int64_t kBaseMillis = 1'700'000'000'000;
constexpr std::int64_t kTtlMillis = 60'000;

void expect_true(bool condition, const std::string& detail) {
  if (!condition) {
    CF_FAIL(detail);
  }
}

void expect_accepted(const MutationResult& result, const std::string& what) {
  if (!result.accepted()) {
    CF_FAIL(what + ": expected acceptance, got outcome=" +
            std::string(to_string(result.outcome)) + " reason=" +
            std::string(to_string(result.reason)) + " error=" + result.error.describe());
  }
}

void expect_invariants_ok(const ClusterCoordinator& coordinator, const std::string& what) {
  const InvariantReport report = coordinator.invariants();
  if (!report.ok()) {
    CF_FAIL(what + ": invariants violated: " + report.describe());
  }
}

/// Thread-safe failure collector. Failures detected on a worker thread are
/// re-raised on the test thread so the harness reports them.
class Failures {
 public:
  void add(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back(std::move(message));
  }

  void check(const std::string& what) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (messages_.empty()) {
      return;
    }
    std::string detail = what + ": " + std::to_string(messages_.size()) + " failure(s)";
    for (const std::string& message : messages_) {
      detail += "\n  " + message;
    }
    CF_FAIL(detail);
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> messages_;
};

[[nodiscard]] ClusterId cluster_named(const std::string& name) {
  return *ClusterId::parse(name);
}

[[nodiscard]] RackId rack_at(std::size_t index) {
  return *RackId::parse("rack-" + std::to_string(index + 1));
}

[[nodiscard]] RackPublisherId publisher_at(std::size_t index) {
  return *RackPublisherId::parse("publisher-" + std::to_string(index + 1));
}

[[nodiscard]] FailureDomainId failure_domain_of(const RackId& rack) {
  return *FailureDomainId::parse("fd-" + rack.value());
}

[[nodiscard]] EvidenceStamp stamp_at(std::int64_t millis) {
  return EvidenceStamp::make(EvidenceProvenance::Measured,
                             Timestamp::from_unix_millis(millis), kTtlMillis);
}

[[nodiscard]] CoordinatorConfig coordinator_config(const ClusterId& cluster) {
  CoordinatorConfig config;
  config.cluster = cluster;
  config.readiness_contract = ReadinessContract{};
  config.bind_address = "127.0.0.1";
  config.port = 0;
  return config;
}

[[nodiscard]] MutationRequest declare_request(const ClusterId& cluster, ClusterEpoch cluster_epoch,
                                              CoordinatorEpoch coordinator_epoch,
                                              std::int64_t millis) {
  MutationRequest request;
  request.kind = MutationKind::DeclareCluster;
  request.cluster = cluster;
  request.authority.cluster_epoch = cluster_epoch;
  request.authority.coordinator_epoch = coordinator_epoch;
  request.declared_lifecycle = ClusterLifecycle::Declared;
  request.readiness_contract = ReadinessContract{};
  request.evidence = stamp_at(millis);
  request.requested_at = Timestamp::from_unix_millis(millis);
  request.reason = "test_declare_cluster";
  return request;
}

[[nodiscard]] MutationRequest add_rack_request(const ClusterId& cluster, ClusterEpoch cluster_epoch,
                                               CoordinatorEpoch coordinator_epoch,
                                               const RackId& rack,
                                               const RackPublisherId& publisher,
                                               const RackAgentBootId& boot,
                                               RackGeneration generation,
                                               PublicationGeneration publication,
                                               const std::string& composition_label,
                                               std::int64_t millis) {
  MutationRequest request;
  request.kind = MutationKind::AddRack;
  request.cluster = cluster;
  request.authority.cluster_epoch = cluster_epoch;
  request.authority.coordinator_epoch = coordinator_epoch;
  request.authority.boot = boot;
  request.authority.publisher = publisher;
  request.authority.rack = rack;
  request.authority.rack_generation = generation;
  request.authority.publication = publication;

  const EvidenceStamp stamp = stamp_at(millis);
  request.evidence = stamp;
  request.requested_at = Timestamp::from_unix_millis(millis);
  request.reason = "test_add_rack";

  RackReference& reference = request.rack_reference;
  reference.rack = rack;
  reference.generation = generation;
  reference.rack_lifecycle = RackLifecycleState::Ready;
  reference.composition.composition_label = composition_label;
  reference.composition.cpu_sockets = 2;
  reference.composition.cpu_cores = 128;
  reference.composition.provenance = EvidenceProvenance::Measured;
  reference.endpoints.push_back(RackEndpoint{*RackEndpointId::parse("fabric0"), std::nullopt,
                                             std::nullopt, ConnectivityClass::DirectFabric,
                                             stamp});
  reference.failure_domain_hints.push_back(
      RackFailureDomainHint{FailureDomainClass::Rack, failure_domain_of(rack), stamp});
  reference.evidence = stamp;
  reference.publisher = publisher;
  reference.publication = publication;
  reference.boot = boot;
  reference.cluster_epoch = cluster_epoch;
  reference.coordinator_epoch = coordinator_epoch;
  reference.health = HealthState::Healthy;
  reference.origin_label = "cluster-fabric-test:rack-agent";
  return request;
}

[[nodiscard]] MutationRequest update_generation_request(
    const ClusterId& cluster, ClusterEpoch cluster_epoch, CoordinatorEpoch coordinator_epoch,
    const RackId& rack, const RackPublisherId& publisher, const RackAgentBootId& boot,
    RackGeneration expected, RackGeneration next, PublicationGeneration publication,
    std::int64_t millis) {
  MutationRequest request;
  request.kind = MutationKind::UpdateRackGeneration;
  request.cluster = cluster;
  request.authority.cluster_epoch = cluster_epoch;
  request.authority.coordinator_epoch = coordinator_epoch;
  request.authority.boot = boot;
  request.authority.publisher = publisher;
  request.authority.rack = rack;
  request.authority.rack_generation = next;
  request.authority.publication = publication;
  const EvidenceStamp stamp = stamp_at(millis);
  request.evidence = stamp;
  request.requested_at = Timestamp::from_unix_millis(millis);
  request.reason = "test_update_rack_generation";
  request.expected_rack_generation = expected;
  request.new_rack_generation = next;
  request.rack_reference.rack = rack;
  request.rack_reference.generation = next;
  request.rack_reference.rack_lifecycle = RackLifecycleState::Ready;
  request.rack_reference.publisher = publisher;
  request.rack_reference.boot = boot;
  request.rack_reference.evidence = stamp;
  request.rack_reference.cluster_epoch = cluster_epoch;
  request.rack_reference.coordinator_epoch = coordinator_epoch;
  return request;
}

[[nodiscard]] MutationRequest link_request(const ClusterId& cluster, ClusterEpoch cluster_epoch,
                                           CoordinatorEpoch coordinator_epoch,
                                           TopologyEpoch topology_epoch,
                                           TopologyGeneration topology_generation,
                                           const RackPublisherId& publisher,
                                           const RackAgentBootId& boot,
                                           const InterRackLinkId& link_id, const RackId& source,
                                           const RackId& destination, std::int64_t millis) {
  MutationRequest request;
  request.kind = MutationKind::PublishInterRackLink;
  request.cluster = cluster;
  request.authority.cluster_epoch = cluster_epoch;
  request.authority.coordinator_epoch = coordinator_epoch;
  request.authority.topology_epoch = topology_epoch;
  request.authority.topology_generation = topology_generation;
  request.authority.boot = boot;
  request.authority.publisher = publisher;
  request.authority.publication = PublicationGeneration::from_raw(1);
  const EvidenceStamp stamp = stamp_at(millis);
  request.evidence = stamp;
  request.requested_at = Timestamp::from_unix_millis(millis);
  request.reason = "test_publish_link";

  InterRackLink link;
  link.id = link_id;
  link.source = source;
  link.destination = destination;
  link.direction = LinkDirection::Bidirectional;
  link.connectivity = ConnectivityClass::DirectFabric;
  link.bandwidth_class = BandwidthClass::High;
  link.latency_class = LatencyClass::VeryLow;
  link.reachability = Reachability::Reachable;
  link.health = HealthState::Healthy;
  link.header.evidence = stamp;
  link.header.generation = DomainGeneration::from_raw(1);
  link.header.publisher = publisher;
  link.header.label = "test-link";
  link.topology_epoch = topology_epoch;
  link.topology_generation = topology_generation;
  link.publication = PublicationGeneration::from_raw(1);
  link.boot = boot;
  request.link = std::move(link);
  return request;
}

[[nodiscard]] std::string describe_result(const MutationResult& result) {
  return "outcome=" + std::string(to_string(result.outcome)) + " reason=" +
         std::string(to_string(result.reason)) + " error=" + result.error.describe();
}

/// Declares the cluster on a running coordinator.
void declare(ClusterCoordinator& coordinator, const ClusterId& cluster) {
  const ClusterState initial = coordinator.state_copy();
  expect_accepted(coordinator.submit(declare_request(cluster, ClusterEpoch::from_raw(1),
                                                     initial.coordinator_epoch, kBaseMillis)),
                  "declare cluster " + cluster.value());
}

}  // namespace

CF_TEST(conflicting_publications_at_one_generation_have_one_winner) {
  constexpr std::size_t kThreads = 8;
  const ClusterId cluster = cluster_named("cluster-fabric-race-conflict");
  ManualClock clock(kBaseMillis);
  ClusterCoordinator coordinator(coordinator_config(cluster), nullptr, &clock);
  expect_true(coordinator.start().ok, "the coordinator did not start");
  declare(coordinator, cluster);

  const RackId rack = *RackId::parse("rack-race-conflict");
  const RackPublisherId publisher = *RackPublisherId::parse("publisher-race-conflict");
  const RackAgentBootId boot = make_rack_agent_boot_id("race-conflict");
  const ClusterState declared = coordinator.state_copy();

  std::vector<MutationResult> results(kThreads);
  std::vector<std::string> labels;
  labels.reserve(kThreads);
  for (std::size_t i = 0; i < kThreads; ++i) {
    labels.push_back("content-" + std::to_string(i));
  }

  std::barrier gate(static_cast<std::ptrdiff_t>(kThreads));
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&coordinator, &gate, &results, &labels, &declared, &cluster, &rack,
                          &publisher, &boot, i]() {
      gate.arrive_and_wait();
      results[i] = coordinator.submit(add_rack_request(
          cluster, declared.epoch, declared.coordinator_epoch, rack, publisher, boot,
          RackGeneration::from_raw(1), PublicationGeneration::from_raw(i + 1), labels[i],
          kBaseMillis));
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  std::string observed;
  std::size_t accepted = 0;
  std::size_t winner = kThreads;
  for (std::size_t i = 0; i < kThreads; ++i) {
    if (!observed.empty()) {
      observed += " ; ";
    }
    observed += "thread" + std::to_string(i) + "(" + labels[i] + ")->" + describe_result(results[i]);
    if (results[i].outcome == MutationOutcome::Accepted) {
      ++accepted;
      winner = i;
    }
  }
  expect_true(accepted == 1, "expected exactly one accepted publication, observed " +
                                 std::to_string(accepted) + ": " + observed);
  for (std::size_t i = 0; i < kThreads; ++i) {
    if (i == winner) {
      continue;
    }
    const RejectionReason reason = results[i].reason;
    const bool typed = results[i].outcome == MutationOutcome::Rejected &&
                       (reason == RejectionReason::Conflict ||
                        reason == RejectionReason::StalePublication);
    expect_true(typed, "loser thread" + std::to_string(i) +
                           " was not rejected with REJECT_CONFLICT or REJECT_STALE_PUBLICATION: " +
                           describe_result(results[i]) + " | all: " + observed);
  }

  const ClusterState state = coordinator.state_copy();
  const auto it = state.racks.find(rack);
  expect_true(it != state.racks.end(), "the winning rack is absent from canonical state");
  expect_true(it->second.reference.composition.composition_label == labels[winner],
              "final state holds composition '" +
                  it->second.reference.composition.composition_label +
                  "' but the accepted publication was '" + labels[winner] + "'");
  expect_true(it->second.reference.generation == RackGeneration::from_raw(1),
              "final rack generation=" + it->second.reference.generation.str());
  expect_true(it->second.authoritative_current, "the winning rack is not authoritative current");
  expect_true(it->second.reference.currentness == RackCurrentness::Current,
              "the winning rack currentness=" +
                  std::string(to_string(it->second.reference.currentness)));
  expect_invariants_ok(coordinator, "after the conflicting publication race");
  coordinator.stop();
}

CF_TEST(concurrent_distinct_racks_match_sequential_replay) {
  constexpr std::size_t kThreads = 8;
  const ClusterId cluster = cluster_named("cluster-fabric-race-distinct");
  ManualClock concurrent_clock(kBaseMillis);
  ManualClock sequential_clock(kBaseMillis);
  ClusterCoordinator concurrent(coordinator_config(cluster), nullptr, &concurrent_clock);
  ClusterCoordinator sequential(coordinator_config(cluster), nullptr, &sequential_clock);
  expect_true(concurrent.start().ok, "the concurrent coordinator did not start");
  expect_true(sequential.start().ok, "the sequential coordinator did not start");
  declare(concurrent, cluster);
  declare(sequential, cluster);
  expect_true(concurrent.state_copy().coordinator_epoch ==
                  sequential.state_copy().coordinator_epoch,
              "the two coordinators started with different coordinator epochs: " +
                  concurrent.state_copy().coordinator_epoch.str() + " vs " +
                  sequential.state_copy().coordinator_epoch.str());

  const ClusterState declared = concurrent.state_copy();
  std::vector<MutationRequest> requests;
  requests.reserve(kThreads);
  for (std::size_t i = 0; i < kThreads; ++i) {
    requests.push_back(add_rack_request(cluster, declared.epoch, declared.coordinator_epoch,
                                        rack_at(i), publisher_at(i),
                                        *RackAgentBootId::parse("boot-distinct-" +
                                                                std::to_string(i + 1)),
                                        RackGeneration::from_raw(1),
                                        PublicationGeneration::from_raw(1),
                                        "composition-" + std::to_string(i), kBaseMillis));
  }

  std::vector<MutationResult> results(kThreads);
  std::barrier gate(static_cast<std::ptrdiff_t>(kThreads));
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t i = 0; i < kThreads; ++i) {
    threads.emplace_back([&concurrent, &gate, &results, &requests, i]() {
      gate.arrive_and_wait();
      results[i] = concurrent.submit(requests[i]);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  for (std::size_t i = 0; i < kThreads; ++i) {
    expect_accepted(results[i], "concurrent add of rack/" + rack_at(i).value());
  }

  std::vector<std::size_t> order;
  order.reserve(kThreads);
  for (std::size_t i = 0; i < kThreads; ++i) {
    order.push_back(i);
  }
  std::sort(order.begin(), order.end(), [&requests](std::size_t lhs, std::size_t rhs) {
    return requests[lhs].rack_reference.rack < requests[rhs].rack_reference.rack;
  });
  for (std::size_t position = 0; position < order.size(); ++position) {
    const std::size_t index = order[position];
    expect_accepted(sequential.submit(requests[index]),
                    "sequential add #" + std::to_string(position) + " of rack/" +
                        requests[index].rack_reference.rack.value());
  }

  const CoordinatorStats stats = concurrent.stats();
  // The accepted mutations are the cluster declaration plus one rack per thread.
  CF_EXPECT_EQ(stats.mutations_accepted, static_cast<std::uint64_t>(kThreads + 1));
  CF_EXPECT_EQ(stats.commits, static_cast<std::uint64_t>(kThreads + 1));
  CF_EXPECT_EQ(stats.mutations_rejected, static_cast<std::uint64_t>(0));
  expect_invariants_ok(concurrent, "after the concurrent membership race");
  expect_invariants_ok(sequential, "after the sequential replay");

  const ClusterState concurrent_state = concurrent.state_copy();
  const ClusterState sequential_state = sequential.state_copy();
  CF_EXPECT_EQ(concurrent_state.racks.size(), kThreads);
  CF_EXPECT_EQ(sequential_state.racks.size(), kThreads);
  const std::string concurrent_digest = semantic_digest_of(concurrent_state);
  const std::string sequential_digest = semantic_digest_of(sequential_state);
  expect_true(concurrent_digest == sequential_digest,
              "concurrent digest " + concurrent_digest + " differs from the sequential digest " +
                  sequential_digest + " (cluster generation " +
                  concurrent_state.generation.str() + " vs " + sequential_state.generation.str() +
                  ", membership generation " + concurrent_state.membership_generation.str() +
                  " vs " + sequential_state.membership_generation.str() + ")");
  concurrent.stop();
  sequential.stop();
}

CF_TEST(concurrent_readers_never_observe_a_torn_or_mismatched_snapshot) {
  constexpr std::size_t kMutations = 200;
  const ClusterId cluster = cluster_named("cluster-fabric-race-readers");
  ManualClock clock(kBaseMillis);
  ClusterCoordinator coordinator(coordinator_config(cluster), nullptr, &clock);
  expect_true(coordinator.start().ok, "the coordinator did not start");
  declare(coordinator, cluster);

  const RackId rack = *RackId::parse("rack-reader-1");
  const RackPublisherId publisher = *RackPublisherId::parse("publisher-reader-1");
  const RackAgentBootId boot = *RackAgentBootId::parse("boot-reader-1");
  const ClusterState declared = coordinator.state_copy();
  expect_accepted(coordinator.submit(add_rack_request(
                      cluster, declared.epoch, declared.coordinator_epoch, rack, publisher, boot,
                      RackGeneration::from_raw(1), PublicationGeneration::from_raw(1),
                      "reader-composition", kBaseMillis)),
                  "add rack/" + rack.value());

  Failures failures;
  std::atomic<bool> writer_done{false};
  std::atomic<std::size_t> observations{0};
  std::atomic<std::size_t> current_observations{0};

  std::thread writer([&coordinator, &cluster, &rack, &publisher, &boot, &failures,
                      &writer_done]() {
    std::uint64_t generation = 1;
    for (std::size_t i = 0; i < kMutations; ++i) {
      const ClusterState state = coordinator.state_copy();
      const MutationResult result = coordinator.submit(update_generation_request(
          cluster, state.epoch, state.coordinator_epoch, rack, publisher, boot,
          RackGeneration::from_raw(generation), RackGeneration::from_raw(generation + 1),
          PublicationGeneration::from_raw(static_cast<std::uint64_t>(i) + 2),
          kBaseMillis + static_cast<std::int64_t>(i)));
      if (!result.accepted()) {
        failures.add("writer mutation " + std::to_string(i) + " was refused: " +
                     describe_result(result));
        break;
      }
      generation += 1;
    }
    writer_done.store(true);
  });

  std::thread reader([&coordinator, &failures, &writer_done, &observations,
                      &current_observations]() {
    std::size_t index = 0;
    do {
      const ClusterSnapshot snapshot = coordinator.snapshot();
      const ClusterState current = coordinator.state_copy();
      const SnapshotValidation validation = snapshot.validate(current);
      ClusterIndexes indexes;
      indexes.rebuild(current);
      const InvariantReport report = check_invariants(current, &indexes);
      if (!report.ok()) {
        failures.add("observation " + std::to_string(index) + " saw an invalid state: " +
                     report.describe());
      }
      if (validation.current) {
        const std::string digest = semantic_digest_of(current);
        if (digest != snapshot.semantic_digest()) {
          failures.add("observation " + std::to_string(index) + " reported current with digest " +
                       snapshot.semantic_digest() + " but current state has digest " + digest);
        }
        current_observations.fetch_add(1);
      }
      observations.fetch_add(1);
      ++index;
    } while (!writer_done.load());
  });

  writer.join();
  reader.join();
  failures.check("concurrent reader");

  const std::size_t observed = observations.load();
  const std::size_t current_observations_seen = current_observations.load();
  expect_true(observed > 0, "the reader never observed the cluster");
  expect_true(current_observations_seen > 0,
              "the reader never observed a current snapshot (" + std::to_string(observed) +
                  " observations)");

  const CoordinatorStats stats = coordinator.stats();
  // The accepted mutations are the declaration, the initial rack admission and
  // kMutations writes.
  CF_EXPECT_EQ(stats.mutations_accepted, static_cast<std::uint64_t>(kMutations + 2));
  CF_EXPECT_EQ(stats.commits, static_cast<std::uint64_t>(kMutations + 2));
  expect_invariants_ok(coordinator, "after the concurrent read/write workload");

  const ClusterSnapshot snapshot = coordinator.snapshot();
  const ClusterState final_state = coordinator.state_copy();
  const SnapshotValidation validation = coordinator.validate(snapshot);
  expect_true(validation.current, "the final snapshot is not current: " + validation.describe());
  expect_true(snapshot.semantic_digest() == semantic_digest_of(final_state),
              "the final snapshot digest " + snapshot.semantic_digest() +
                  " does not match current state digest " + semantic_digest_of(final_state));
  const auto it = final_state.racks.find(rack);
  expect_true(it != final_state.racks.end(), "rack/" + rack.value() + " is absent");
  expect_true(it->second.reference.generation == RackGeneration::from_raw(kMutations + 1),
              "final rack generation=" + it->second.reference.generation.str() + " expected " +
                  std::to_string(kMutations + 1));
  coordinator.stop();
}

CF_TEST(deterministic_scenario_repeats_to_one_digest) {
  constexpr std::size_t kRepeats = 100;
  constexpr std::size_t kRacks = 3;
  const ClusterId cluster = cluster_named("cluster-fabric-race-repeat");
  std::string first_digest;
  for (std::size_t repeat = 0; repeat < kRepeats; ++repeat) {
    ManualClock clock(kBaseMillis);
    ClusterCoordinator coordinator(coordinator_config(cluster), nullptr, &clock);
    expect_true(coordinator.start().ok,
                "repeat " + std::to_string(repeat) + ": the coordinator did not start");
    declare(coordinator, cluster);
    for (std::size_t i = 0; i < kRacks; ++i) {
      const ClusterState state = coordinator.state_copy();
      expect_accepted(coordinator.submit(add_rack_request(
                          cluster, state.epoch, state.coordinator_epoch, rack_at(i),
                          publisher_at(i),
                          *RackAgentBootId::parse("boot-repeat-" + std::to_string(i + 1)),
                          RackGeneration::from_raw(1), PublicationGeneration::from_raw(1),
                          "composition-" + std::to_string(i), kBaseMillis)),
                      "repeat " + std::to_string(repeat) + ": add rack/" + rack_at(i).value());
    }
    const ClusterState state = coordinator.state_copy();
    expect_accepted(coordinator.submit(link_request(
                        cluster, state.epoch, state.coordinator_epoch, state.topology_epoch,
                        state.topology_generation, publisher_at(0),
                        *RackAgentBootId::parse("boot-repeat-1"), *InterRackLinkId::parse("link-1"),
                        rack_at(0), rack_at(1), kBaseMillis)),
                    "repeat " + std::to_string(repeat) + ": publish link-1");

    const std::string digest = semantic_digest_of(coordinator.state_copy());
    if (repeat == 0) {
      first_digest = digest;
    } else {
      expect_true(digest == first_digest,
                  "repeat " + std::to_string(repeat) + " produced digest " + digest +
                      " but the first repeat produced " + first_digest);
    }
    expect_invariants_ok(coordinator, "repeat " + std::to_string(repeat));
    coordinator.stop();
  }
  CF_EXPECT_EQ(first_digest.size(), static_cast<std::size_t>(16));
}

int main() { return cf_test::run("test_race_deterministic"); }
