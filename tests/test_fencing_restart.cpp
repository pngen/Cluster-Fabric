// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Coordinator restart, durable recovery, fencing and revalidation.
//
// The property under test: restarting a coordinator on an existing durable
// container advances the coordinator epoch by exactly one, fences every
// recovered process incarnation with reason "coordinator_restart", and refuses
// to treat any recovered dynamic observation as current. Requests authored
// under the superseded epoch or by a fenced incarnation are rejected with
// typed reasons, and the cluster only returns to READY once every rack has
// republished current evidence under fresh authority.

#include "harness.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "cluster_fabric/cluster_fabric.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace cluster_fabric;

namespace {

/// Fixed instants: every assertion below is independent of wall-clock time.
constexpr std::int64_t kBaseMillis = 1'700'000'000'000;
constexpr std::int64_t kTtlMillis = 60'000;
constexpr std::size_t kRackCount = 3;
constexpr std::size_t kLinkCount = 2;

[[nodiscard]] std::uint32_t current_process_id() {
#if defined(_WIN32)
  return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint32_t>(::getpid());
#endif
}

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

void expect_rejected_with(const MutationResult& result, RejectionReason expected,
                          const std::string& what) {
  if (result.outcome != MutationOutcome::Rejected || result.reason != expected) {
    CF_FAIL(what + ": expected " + std::string(to_string(expected)) + " but got outcome=" +
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

/// A unique temporary directory created by the test and removed at the end.
class TempDirectory {
 public:
  TempDirectory() {
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
      const std::filesystem::path candidate =
          base / ("cf-fencing-restart-" + std::to_string(current_process_id()) + "-" +
                  std::to_string(attempt));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error)) {
        path_ = candidate;
        return;
      }
    }
    CF_FAIL("could not create a unique temporary directory under " + base.string());
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] std::string file(const std::string& name) const {
    return (path_ / name).string();
  }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] ClusterId test_cluster() {
  return *ClusterId::parse("cluster-fabric-fencing");
}

[[nodiscard]] RackId rack_at(std::size_t index) {
  return *RackId::parse("rack-" + std::to_string(index + 1));
}

[[nodiscard]] RackPublisherId publisher_at(std::size_t index) {
  return *RackPublisherId::parse("publisher-" + std::to_string(index + 1));
}

[[nodiscard]] InterRackLinkId link_at(std::size_t index) {
  return *InterRackLinkId::parse("link-" + std::to_string(index + 1));
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
  reference.composition.composition_label = "rack-composition-" + rack.value();
  reference.composition.cpu_sockets = 2;
  reference.composition.cpu_cores = 128;
  reference.composition.nic_count = 2;
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
  request.topology_reason = "test_publish_link";

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

[[nodiscard]] MutationRequest revalidate_request(const ClusterId& cluster,
                                                 ClusterEpoch cluster_epoch,
                                                 CoordinatorEpoch coordinator_epoch,
                                                 const std::vector<RackId>& racks,
                                                 std::int64_t millis) {
  MutationRequest request;
  request.kind = MutationKind::RevalidateRecoveredState;
  request.cluster = cluster;
  request.authority.cluster_epoch = cluster_epoch;
  request.authority.coordinator_epoch = coordinator_epoch;
  request.evidence = stamp_at(millis);
  request.requested_at = Timestamp::from_unix_millis(millis);
  request.reason = "test_revalidate";
  request.racks = racks;
  return request;
}

[[nodiscard]] bool read_bytes(const std::string& path, std::string& out) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  return stream.good() || stream.eof();
}

[[nodiscard]] bool write_bytes(const std::string& path, const std::string& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.is_open()) {
    return false;
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  stream.flush();
  return stream.good();
}

[[nodiscard]] std::string joined(const std::vector<std::string>& values) {
  std::string out;
  for (const std::string& value : values) {
    if (!out.empty()) {
      out += " | ";
    }
    out += value;
  }
  return out;
}

/// Fences every rack of the cluster with a fresh incarnation and republishes
/// current evidence, then verifies the cluster is READY again.
void republish_all_racks(ClusterCoordinator& coordinator, const ClusterId& cluster,
                         std::int64_t millis, const std::string& label) {
  const ClusterState before = coordinator.state_copy();
  for (std::size_t i = 0; i < kRackCount; ++i) {
    const RackAgentBootId boot = make_rack_agent_boot_id(label);
    const MutationResult result = coordinator.submit(
        add_rack_request(cluster, before.epoch, before.coordinator_epoch, rack_at(i),
                         publisher_at(i), boot, RackGeneration::from_raw(1),
                         PublicationGeneration::from_raw(2), millis));
    expect_accepted(result, label + ": republish rack/" + rack_at(i).value());
    const ClusterState state = coordinator.state_copy();
    const auto it = state.racks.find(rack_at(i));
    expect_true(it != state.racks.end(), label + ": rack disappeared after republish");
    expect_true(it->second.authoritative_current,
                label + ": rack/" + rack_at(i).value() + " is not authoritative current after "
                "republishing current evidence");
    expect_true(it->second.reference.currentness == RackCurrentness::Current,
                label + ": rack/" + rack_at(i).value() + " currentness=" +
                    std::string(to_string(it->second.reference.currentness)));
    expect_true(it->second.reference.evidence.is_current(),
                label + ": rack/" + rack_at(i).value() + " evidence is not current");
  }
}

/// Asserts the three declared racks are present and current at generation 1.
void expect_racks_current(const ClusterState& state, const std::string& what) {
  for (std::size_t i = 0; i < kRackCount; ++i) {
    const auto it = state.racks.find(rack_at(i));
    expect_true(it != state.racks.end(), what + ": rack/" + rack_at(i).value() + " is absent");
    const RackRecord& record = it->second;
    expect_true(record.membership == RackMembershipState::Active,
                what + ": rack/" + rack_at(i).value() + " membership=" +
                    std::string(to_string(record.membership)));
    expect_true(record.authoritative_current,
                what + ": rack/" + rack_at(i).value() + " is not authoritative current");
    expect_true(record.reference.generation == RackGeneration::from_raw(1),
                what + ": rack/" + rack_at(i).value() + " generation=" +
                    record.reference.generation.str());
  }
}

}  // namespace

CF_TEST(restart_fences_recovered_state_and_requires_revalidation) {
  TempDirectory directory;
  const std::string container = directory.file("cluster-state.bin");
  const ClusterId cluster = test_cluster();
  ManualClock clock(kBaseMillis);

  CoordinatorEpoch first_coordinator_epoch;
  std::string first_digest;
  std::vector<RackAgentBootId> first_boots;

  {
    FilePersistenceStore store(container);
    ClusterCoordinator coordinator(coordinator_config(cluster), &store, &clock);
    const CoordinatorStartOutcome outcome = coordinator.start();
    expect_true(outcome.ok, "first start failed: " + outcome.error.describe());
    CF_EXPECT_EQ(outcome.status, ProtocolStatus::Ok);
    CF_EXPECT(coordinator.running());
    CF_EXPECT_NE(coordinator.port(), static_cast<std::uint16_t>(0));

    const ClusterState initial = coordinator.state_copy();
    first_coordinator_epoch = initial.coordinator_epoch;
    expect_true(first_coordinator_epoch.known(), "the first coordinator epoch is unknown");
    expect_true(!initial.epoch.known(), "a fresh container must start undeclared");

    expect_accepted(coordinator.submit(declare_request(cluster, ClusterEpoch::from_raw(1),
                                                       first_coordinator_epoch, kBaseMillis)),
                    "declare cluster");

    for (std::size_t i = 0; i < kRackCount; ++i) {
      const RackAgentBootId boot = make_rack_agent_boot_id("boot-first");
      first_boots.push_back(boot);
      const ClusterState state = coordinator.state_copy();
      expect_accepted(coordinator.submit(add_rack_request(
                          cluster, state.epoch, state.coordinator_epoch, rack_at(i),
                          publisher_at(i), boot, RackGeneration::from_raw(1),
                          PublicationGeneration::from_raw(1), kBaseMillis)),
                      "add rack/" + rack_at(i).value());
    }

    for (std::size_t i = 0; i < kLinkCount; ++i) {
      const ClusterState state = coordinator.state_copy();
      expect_accepted(coordinator.submit(link_request(
                          cluster, state.epoch, state.coordinator_epoch, state.topology_epoch,
                          state.topology_generation, publisher_at(i), first_boots[i], link_at(i),
                          rack_at(i), rack_at(i + 1), kBaseMillis)),
                      "publish link-" + std::to_string(i + 1));
    }

    const ClusterState state = coordinator.state_copy();
    CF_EXPECT_EQ(state.racks.size(), kRackCount);
    CF_EXPECT_EQ(state.links.size(), kLinkCount);
    expect_racks_current(state, "before restart");
    expect_true(state.lifecycle == ClusterLifecycle::Ready,
                "before restart: lifecycle=" + std::string(to_string(state.lifecycle)));
    expect_true(coordinator.readiness().satisfied, "before restart: readiness is not satisfied");
    expect_invariants_ok(coordinator, "before restart");

    const ClusterSnapshot snapshot = coordinator.snapshot();
    first_digest = snapshot.semantic_digest();
    CF_EXPECT_EQ(first_digest.size(), static_cast<std::size_t>(16));
    expect_true(coordinator.validate(snapshot).ok(), "before restart: snapshot is not valid");

    coordinator.stop();
    CF_EXPECT(!coordinator.running());
  }

  // --- restart on the same container ---------------------------------------
  {
    FilePersistenceStore store(container);
    ClusterCoordinator coordinator(coordinator_config(cluster), &store, &clock);
    const CoordinatorStartOutcome outcome = coordinator.start();
    expect_true(outcome.ok, "restart failed: " + outcome.error.describe());

    const CoordinatorEpoch expected_epoch =
        CoordinatorEpoch::from_raw(first_coordinator_epoch.value() + 1);
    const RecoveryReport& recovery = coordinator.recovery();
    expect_true(recovery.attempted, "recovery was not attempted");
    expect_true(recovery.loaded, "durable state was not loaded");
    CF_EXPECT_EQ(recovery.status, PersistenceStatus::Ok);
    expect_true(recovery.coordinator_epoch_advanced, "coordinator epoch did not advance");
    CF_EXPECT_EQ(recovery.previous_coordinator_epoch, first_coordinator_epoch);
    CF_EXPECT_EQ(recovery.current_coordinator_epoch, expected_epoch);
    expect_true(expected_epoch.value() == first_coordinator_epoch.value() + 1,
                "coordinator epoch did not advance by exactly one: previous=" +
                    first_coordinator_epoch.str() + " current=" + expected_epoch.str());
    CF_EXPECT_EQ(recovery.racks_recovered, kRackCount);
    CF_EXPECT_EQ(recovery.links_recovered, kLinkCount);
    CF_EXPECT_EQ(recovery.domains_recovered, static_cast<std::size_t>(0));
    CF_EXPECT_EQ(recovery.retired_recovered, static_cast<std::size_t>(0));
    CF_EXPECT_EQ(recovery.fenced_recovered, kRackCount);
    expect_true(recovery.revalidation_required, "recovery did not require revalidation");
    expect_true(std::is_sorted(recovery.notes.begin(), recovery.notes.end()),
                "recovery notes are not sorted: " + joined(recovery.notes));
    bool loaded_note = false;
    for (const std::string& note : recovery.notes) {
      if (note.find("durable state loaded") != std::string::npos) {
        loaded_note = true;
      }
    }
    expect_true(loaded_note, "recovery notes do not mention the loaded container: " +
                                 joined(recovery.notes));

    const ClusterState state = coordinator.state_copy();
    CF_EXPECT_EQ(state.coordinator_epoch, expected_epoch);
    CF_EXPECT_EQ(state.epoch, ClusterEpoch::from_raw(1));
    CF_EXPECT_EQ(state.racks.size(), kRackCount);
    CF_EXPECT_EQ(state.links.size(), kLinkCount);
    expect_true(state.lifecycle == ClusterLifecycle::RevalidationRequired,
                "recovered lifecycle=" + std::string(to_string(state.lifecycle)));

    for (std::size_t i = 0; i < kRackCount; ++i) {
      const RackId rack = rack_at(i);
      const auto it = state.racks.find(rack);
      expect_true(it != state.racks.end(), "recovered rack/" + rack.value() + " is absent");
      const RackRecord& record = it->second;
      const std::string label = "recovered rack/" + rack.value();
      expect_true(!record.authoritative_current,
                  label + " is authoritative current after a coordinator restart");
      expect_true(record.reference.currentness == RackCurrentness::RevalidationRequired,
                  label + " currentness=" + std::string(to_string(record.reference.currentness)));
      expect_true(record.reference.evidence.requires_revalidation(),
                  label + " evidence does not require revalidation (freshness=" +
                      std::string(to_string(record.reference.evidence.freshness)) + ")");
      expect_true(record.reference.evidence.provenance == EvidenceProvenance::Reconstructed,
                  label + " evidence provenance=" +
                      std::string(to_string(record.reference.evidence.provenance)));
      expect_true(record.reference.health == HealthState::Unknown,
                  label + " health=" + std::string(to_string(record.reference.health)));
      expect_true(record.membership == RackMembershipState::Active,
                  label + " membership=" + std::string(to_string(record.membership)));
      expect_true(record.reference.generation == RackGeneration::from_raw(1),
                  label + " generation=" + record.reference.generation.str());
      expect_true(!record.non_authoritative_reason.empty(),
                  label + " has no non-authoritative reason");

      bool fenced = false;
      std::string fence_reason;
      for (const FencedAuthority& entry : state.fenced_authorities) {
        if (entry.boot == first_boots[i]) {
          fenced = true;
          fence_reason = entry.reason;
          CF_EXPECT_EQ(entry.rack, rack);
        }
      }
      expect_true(fenced, "recovered boot " + first_boots[i].value() +
                              " is absent from fenced_authorities");
      expect_true(fence_reason == "coordinator_restart",
                  "fence reason for " + first_boots[i].value() + " is '" + fence_reason + "'");
    }

    for (const auto& entry : state.links) {
      const InterRackLink& link = entry.second;
      const std::string label = "recovered link/" + entry.first.value();
      expect_true(link.reachability == Reachability::Unknown,
                  label + " reachability=" + std::string(to_string(link.reachability)));
      expect_true(link.health == HealthState::Unknown,
                  label + " health=" + std::string(to_string(link.health)));
      expect_true(link.header.evidence.requires_revalidation(),
                  label + " evidence does not require revalidation");
    }

    const ReadinessEvaluation readiness = coordinator.readiness();
    expect_true(!readiness.satisfied, "recovered readiness is satisfied");
    expect_true(readiness.lifecycle == ClusterLifecycle::RevalidationRequired,
                "recovered readiness lifecycle=" +
                    std::string(to_string(readiness.lifecycle)));
    bool revalidation_blocker = false;
    for (const ReadinessBlocker& blocker : readiness.blockers) {
      if (blocker.code == "rack_revalidation_required") {
        revalidation_blocker = true;
      }
    }
    expect_true(revalidation_blocker, "no rack_revalidation_required blocker was reported");
    expect_invariants_ok(coordinator, "after restart");

    const SnapshotValidation recovered_validation = coordinator.validate(coordinator.snapshot());
    expect_true(!recovered_validation.current,
                "the recovered snapshot claims to be current: " + recovered_validation.describe());

    // A request authored under the superseded coordinator epoch is fenced.
    const MutationResult stale_epoch = coordinator.submit(add_rack_request(
        cluster, state.epoch, first_coordinator_epoch, rack_at(0), publisher_at(0),
        make_rack_agent_boot_id("boot-stale"), RackGeneration::from_raw(1),
        PublicationGeneration::from_raw(1), kBaseMillis));
    expect_rejected_with(stale_epoch, RejectionReason::StaleCoordinatorEpoch,
                         "request authored under the old coordinator epoch");
    CF_EXPECT_EQ(std::string(to_string(stale_epoch.reason)),
                 std::string("REJECT_STALE_COORDINATOR_EPOCH"));

    // A request from a recovered (now fenced) incarnation is fenced.
    const MutationResult fenced_boot = coordinator.submit(add_rack_request(
        cluster, state.epoch, expected_epoch, rack_at(0), publisher_at(0), first_boots[0],
        RackGeneration::from_raw(1), PublicationGeneration::from_raw(2), kBaseMillis));
    expect_rejected_with(fenced_boot, RejectionReason::StaleRackBoot,
                         "request from a fenced boot");
    CF_EXPECT_EQ(std::string(to_string(fenced_boot.reason)),
                 std::string("REJECT_STALE_RACK_BOOT"));

    // Revalidation cannot succeed while evidence is still missing.
    const MutationResult incomplete =
        coordinator.submit(revalidate_request(cluster, state.epoch, expected_epoch, {}, kBaseMillis));
    expect_true(incomplete.outcome == MutationOutcome::RevalidationRequired,
                "incomplete revalidation outcome=" + std::string(to_string(incomplete.outcome)) +
                    " reason=" + std::string(to_string(incomplete.reason)));

    // Republish under the new coordinator epoch and revalidate.
    republish_all_racks(coordinator, cluster, kBaseMillis, "boot-second");
    const ClusterState republished = coordinator.state_copy();
    const MutationResult revalidated = coordinator.submit(revalidate_request(
        cluster, republished.epoch, republished.coordinator_epoch,
        {rack_at(0), rack_at(1), rack_at(2)}, kBaseMillis));
    expect_accepted(revalidated, "revalidate recovered state");

    const ClusterState ready_state = coordinator.state_copy();
    expect_true(ready_state.lifecycle == ClusterLifecycle::Ready,
                "republished lifecycle=" + std::string(to_string(ready_state.lifecycle)));
    expect_racks_current(ready_state, "after republish");
    expect_true(coordinator.readiness().satisfied,
                "readiness is not satisfied after republishing every rack");
    expect_invariants_ok(coordinator, "after republish");

    for (const RackAgentBootId& boot : first_boots) {
      expect_true(ready_state.is_boot_fenced(boot),
                  "old boot " + boot.value() + " is no longer fenced");
    }
    for (std::size_t i = 0; i < kRackCount; ++i) {
      const auto it = ready_state.racks.find(rack_at(i));
      expect_true(it != ready_state.racks.end(), "rack/" + rack_at(i).value() + " is absent");
      expect_true(!ready_state.is_boot_fenced(it->second.reference.boot),
                  "fresh boot " + it->second.reference.boot.value() + " is fenced");
    }

    const ClusterSnapshot snapshot = coordinator.snapshot();
    const SnapshotValidation validation = coordinator.validate(snapshot);
    expect_true(validation.current, "snapshot is not current: " + validation.describe());
    expect_true(validation.consumable, "snapshot is not consumable: " + validation.describe());
    expect_true(validation.reasons.empty(),
                "snapshot reports stale reasons: " + validation.describe());
    expect_true(!snapshot.semantic_digest().empty(), "snapshot digest is empty");
    expect_true(snapshot.semantic_digest() != first_digest,
                "the digest did not change across the restart");

    coordinator.stop();
  }
}

CF_TEST(restart_cycles_advance_coordinator_epoch_monotonically) {
  TempDirectory directory;
  const std::string container = directory.file("cluster-state.bin");
  const ClusterId cluster = test_cluster();
  ManualClock clock(kBaseMillis);

  constexpr std::size_t kCycles = 6;
  std::vector<CoordinatorEpoch> observed;

  for (std::size_t cycle = 0; cycle < kCycles; ++cycle) {
    FilePersistenceStore store(container);
    ClusterCoordinator coordinator(coordinator_config(cluster), &store, &clock);
    const CoordinatorStartOutcome outcome = coordinator.start();
    expect_true(outcome.ok, "cycle " + std::to_string(cycle) + " start failed: " +
                                outcome.error.describe());

    const ClusterState state = coordinator.state_copy();
    const CoordinatorEpoch epoch = state.coordinator_epoch;
    expect_true(epoch.known(), "cycle " + std::to_string(cycle) + " coordinator epoch is unknown");
    observed.push_back(epoch);

    if (cycle == 0) {
      expect_true(!state.epoch.known(),
                  "the first cycle must begin with an undeclared cluster");
      expect_accepted(coordinator.submit(declare_request(cluster, ClusterEpoch::from_raw(1), epoch,
                                                         kBaseMillis)),
                      "cycle 0: declare cluster");
      for (std::size_t i = 0; i < kRackCount; ++i) {
        const ClusterState current = coordinator.state_copy();
        expect_accepted(coordinator.submit(add_rack_request(
                            cluster, current.epoch, current.coordinator_epoch, rack_at(i),
                            publisher_at(i), make_rack_agent_boot_id("boot-cycle0"),
                            RackGeneration::from_raw(1), PublicationGeneration::from_raw(1),
                            kBaseMillis)),
                        "cycle 0: add rack/" + rack_at(i).value());
      }
    } else {
      const CoordinatorEpoch previous = observed[cycle - 1];
      expect_true(epoch > previous, "cycle " + std::to_string(cycle) + ": coordinator epoch " +
                                        epoch.str() + " is not greater than " + previous.str());
      expect_true(epoch.value() == previous.value() + 1,
                  "cycle " + std::to_string(cycle) + ": coordinator epoch advanced by " +
                      std::to_string(epoch.value() - previous.value()) + " (previous=" +
                      previous.str() + " current=" + epoch.str() + ")");
      republish_all_racks(coordinator, cluster, kBaseMillis + static_cast<std::int64_t>(cycle),
                          "boot-cycle" + std::to_string(cycle));
    }

    const ClusterState final_state = coordinator.state_copy();
    expect_true(final_state.lifecycle == ClusterLifecycle::Ready,
                "cycle " + std::to_string(cycle) + ": lifecycle=" +
                    std::string(to_string(final_state.lifecycle)));
    expect_invariants_ok(coordinator, "cycle " + std::to_string(cycle));
    coordinator.stop();
    CF_EXPECT(!coordinator.running());
  }

  CF_EXPECT_EQ(observed.size(), kCycles);
  for (std::size_t i = 1; i < observed.size(); ++i) {
    expect_true(observed[i] > observed[i - 1],
                "coordinator epochs are not strictly increasing at index " + std::to_string(i) +
                    ": " + observed[i - 1].str() + " then " + observed[i].str());
  }
}

CF_TEST(corrupt_container_is_rejected_and_starts_undeclared) {
  TempDirectory directory;
  const std::string container = directory.file("cluster-state.bin");
  const ClusterId cluster = test_cluster();
  ManualClock clock(kBaseMillis);
  RackAgentBootId durable_boot;

  {
    FilePersistenceStore store(container);
    ClusterCoordinator coordinator(coordinator_config(cluster), &store, &clock);
    expect_true(coordinator.start().ok, "the first start failed");
    const CoordinatorEpoch epoch = coordinator.state_copy().coordinator_epoch;
    expect_accepted(coordinator.submit(declare_request(cluster, ClusterEpoch::from_raw(1), epoch,
                                                       kBaseMillis)),
                    "declare cluster");
    durable_boot = make_rack_agent_boot_id("boot-durable");
    const ClusterState state = coordinator.state_copy();
    expect_accepted(coordinator.submit(add_rack_request(
                        cluster, state.epoch, state.coordinator_epoch, rack_at(0), publisher_at(0),
                        durable_boot, RackGeneration::from_raw(1), PublicationGeneration::from_raw(1),
                        kBaseMillis)),
                    "add rack/" + rack_at(0).value());
    expect_invariants_ok(coordinator, "before corruption");
    coordinator.stop();
  }

  std::string bytes;
  expect_true(read_bytes(container, bytes), "could not read the durable container " + container);
  expect_true(bytes.size() > 32, "the durable container is unexpectedly small: " +
                                     std::to_string(bytes.size()) + " bytes");
  const std::size_t offset = bytes.size() / 2;
  bytes[offset] = static_cast<char>(static_cast<unsigned char>(bytes[offset]) ^ 0xffu);
  expect_true(write_bytes(container, bytes), "could not rewrite the corrupted container");

  FilePersistenceStore store(container);
  ClusterCoordinator coordinator(coordinator_config(cluster), &store, &clock);
  const CoordinatorStartOutcome outcome = coordinator.start();
  expect_true(outcome.ok, "the coordinator must still start on a corrupt container: " +
                              outcome.error.describe());

  const RecoveryReport& recovery = coordinator.recovery();
  expect_true(recovery.attempted, "recovery was not attempted");
  expect_true(!recovery.loaded, "corrupt durable state was reported as loaded");
  expect_true(recovery.status == PersistenceStatus::ChecksumMismatch,
              "expected PersistenceStatus::ChecksumMismatch but got " +
                  std::string(to_string(recovery.status)));
  expect_true(!recovery.coordinator_epoch_advanced,
              "the coordinator epoch advanced even though nothing was loaded");
  CF_EXPECT_EQ(recovery.racks_recovered, static_cast<std::size_t>(0));
  bool rejected_note = false;
  for (const std::string& note : recovery.notes) {
    if (note.find("durable container rejected") != std::string::npos) {
      rejected_note = true;
    }
  }
  expect_true(rejected_note, "recovery notes do not report the rejected container: " +
                                 joined(recovery.notes));

  const ClusterState state = coordinator.state_copy();
  expect_true(!state.epoch.known(),
              "the cluster is declared after a corrupt container was rejected");
  expect_true(state.racks.empty(),
              "corrupt durable state was trusted: " + std::to_string(state.racks.size()) +
                  " rack(s) recovered");
  expect_true(state.fenced_authorities.empty(),
              "corrupt durable state was trusted: fenced authorities were recovered");
  expect_true(!state.is_boot_fenced(durable_boot),
              "the corrupt container's boot identity was trusted");
  expect_invariants_ok(coordinator, "after corrupt container rejection");

  expect_accepted(coordinator.submit(declare_request(cluster, ClusterEpoch::from_raw(1),
                                                     state.coordinator_epoch, kBaseMillis)),
                  "declare a fresh cluster after the corrupt container was rejected");
  const ClusterState fresh = coordinator.state_copy();
  expect_true(fresh.epoch == ClusterEpoch::from_raw(1),
              "fresh cluster epoch=" + fresh.epoch.str());
  expect_true(fresh.racks.empty(), "the fresh cluster unexpectedly has racks");
  expect_invariants_ok(coordinator, "after fresh declaration");
  coordinator.stop();
}

int main() { return cf_test::run("test_fencing_restart"); }
