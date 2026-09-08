// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The in-process rack agent contract: connect/register, publish, supersede,
// revalidate, heartbeat, snapshot round trips and shutdown.
//
// Every assertion below is a property of the product: if any of them is
// violated the test fails with the observed values.

#include "harness.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

namespace {

constexpr std::int64_t kBaseMillis = 1'700'000'000'000;
constexpr std::size_t kPollAttempts = 200;
constexpr std::size_t kPollIntervalMillis = 25;

void expect_true(bool condition, const std::string& detail) {
  if (!condition) {
    CF_FAIL(detail);
  }
}

void expect_accepted(const MutationResult& result, const std::string& what) {
  if (result.accepted()) {
    return;
  }
  if (result.outcome == MutationOutcome::Rejected && result.reason == RejectionReason::None) {
    CF_FAIL(what + ": the coordinator returned the untyped sentinel rejection "
                   "(outcome=REJECTED reason=NONE error=" +
            result.error.describe() + "), which means an internal validation helper returned a "
            "default MutationResult instead of a success result");
  }
  CF_FAIL(what + ": expected acceptance, got outcome=" +
          std::string(to_string(result.outcome)) + " reason=" +
          std::string(to_string(result.reason)) + " error=" + result.error.describe());
}

[[nodiscard]] std::string describe_result(const MutationResult& result) {
  return "outcome=" + std::string(to_string(result.outcome)) + " reason=" +
         std::string(to_string(result.reason)) + " error=" + result.error.describe();
}

[[nodiscard]] ClusterId test_cluster() {
  return *ClusterId::parse("cluster-fabric-rack-agent");
}

[[nodiscard]] EvidenceStamp stamp_at(std::int64_t millis) {
  return EvidenceStamp::make(EvidenceProvenance::Measured,
                             Timestamp::from_unix_millis(millis), 60'000);
}

[[nodiscard]] MutationRequest declare_request(const ClusterId& cluster,
                                              CoordinatorEpoch coordinator_epoch) {
  MutationRequest request;
  request.kind = MutationKind::DeclareCluster;
  request.cluster = cluster;
  request.authority.cluster_epoch = ClusterEpoch::from_raw(1);
  request.authority.coordinator_epoch = coordinator_epoch;
  request.declared_lifecycle = ClusterLifecycle::Declared;
  request.readiness_contract = ReadinessContract{};
  request.evidence = stamp_at(kBaseMillis);
  request.requested_at = Timestamp::from_unix_millis(kBaseMillis);
  request.reason = "test_declare_cluster";
  return request;
}

[[nodiscard]] RackAgentConfig agent_config(const ClusterId& cluster, const RackId& rack,
                                           const RackPublisherId& publisher,
                                           RackGeneration generation, std::uint16_t port) {
  RackAgentConfig config;
  config.cluster = cluster;
  config.rack = rack;
  config.generation = generation;
  config.publisher = publisher;
  config.coordinator_host = "127.0.0.1";
  config.coordinator_port = port;
  config.publication_ttl_millis = 30'000;
  config.origin_label = "cluster-fabric-test:rack-agent";
  config.rack_lifecycle = RackLifecycleState::Ready;
  config.health = HealthState::Healthy;
  config.reachability = Reachability::Reachable;
  config.composition.composition_label = "test-composition";
  config.composition.cpu_sockets = 2;
  config.composition.cpu_cores = 128;
  config.composition.nic_count = 2;
  config.composition.provenance = EvidenceProvenance::Reported;
  AcceleratorClassSummary accelerator;
  accelerator.vendor = AcceleratorVendor::Nvidia;
  accelerator.family = "test-family";
  accelerator.device_count = 4;
  accelerator.device_memory_bytes = 16ull * 1024 * 1024 * 1024;
  config.composition.accelerators.push_back(accelerator);
  RackEndpoint endpoint;
  endpoint.id = *RackEndpointId::parse("fabric0");
  endpoint.connectivity = ConnectivityClass::DirectFabric;
  endpoint.evidence = stamp_at(kBaseMillis);
  config.endpoints.push_back(endpoint);
  RackFailureDomainHint hint;
  hint.klass = FailureDomainClass::Rack;
  hint.id = *FailureDomainId::parse("fd-" + rack.value());
  hint.evidence = stamp_at(kBaseMillis);
  config.failure_domain_hints.push_back(hint);
  return config;
}

}  // namespace

CF_TEST(rack_agent_lifecycle_against_a_live_coordinator) {
  const ClusterId cluster = test_cluster();
  const RackId rack = *RackId::parse("rack-agent-1");
  const RackPublisherId publisher = *RackPublisherId::parse("rack-publisher-agent-1");
  const RackGeneration generation = RackGeneration::from_raw(1);

  CoordinatorConfig config;
  config.cluster = cluster;
  config.readiness_contract = ReadinessContract{};
  config.bind_address = "127.0.0.1";
  config.port = 0;
  ClusterCoordinator coordinator(config, nullptr, nullptr);
  const CoordinatorStartOutcome started = coordinator.start();
  expect_true(started.ok, "the coordinator did not start: " + started.error.describe());
  const std::uint16_t port = coordinator.port();
  expect_true(port != 0, "the coordinator did not report a bound port");
  expect_accepted(coordinator.submit(
                      declare_request(cluster, coordinator.state_copy().coordinator_epoch)),
                  "declare cluster");

  RackAgent agent(agent_config(cluster, rack, publisher, generation, port));

  // --- connect: transport, handshake and registration ----------------------
  const bool connected = agent.connect();
  expect_true(connected,
              "RackAgent::connect() returned false: state=" +
                  std::string(to_string(agent.status().state)) + " transport=" +
                  std::string(to_string(agent.status().transport)) + " rejection=" +
                  std::string(to_string(agent.status().rejection)) + " detail=" +
                  agent.status().detail);
  expect_true(agent.status().registered, "the agent did not report itself registered");
  expect_true(agent.status().connected, "the agent did not report a connected transport");
  expect_true(agent.status().state == RackAgentState::Ready,
              "agent state after connect=" + std::string(to_string(agent.status().state)));
  expect_true(agent.status().transport == ProtocolStatus::Ok,
              "agent transport after connect=" +
                  std::string(to_string(agent.status().transport)));
  expect_true(agent.boot().known(), "the agent has no boot identity");

  {
    const ClusterState state = coordinator.state_copy();
    const auto it = state.racks.find(rack);
    expect_true(it != state.racks.end(),
                "the coordinator has no record for rack/" + rack.value() +
                    " after the agent registered");
    expect_true(it->second.reference.boot == agent.boot(),
                "the registered record carries boot " + it->second.reference.boot.value() +
                    " but the agent reports " + agent.boot().value());
  }

  // --- publish -------------------------------------------------------------
  const MutationResult published = agent.publish();
  expect_accepted(published, "RackAgent::publish()");
  {
    const ClusterState state = coordinator.state_copy();
    const auto it = state.racks.find(rack);
    expect_true(it != state.racks.end(), "rack/" + rack.value() + " is absent after publish");
    const RackRecord& record = it->second;
    expect_true(record.reference.generation == generation,
                "published generation=" + record.reference.generation.str() + " expected " +
                    generation.str());
    expect_true(record.membership == RackMembershipState::Active,
                "published membership=" + std::string(to_string(record.membership)));
    expect_true(record.reference.currentness == RackCurrentness::Current,
                "published currentness=" + std::string(to_string(record.reference.currentness)));
    expect_true(record.authoritative_current, "the published rack is not authoritative current");
    expect_true(record.reference.boot == agent.boot(),
                "the authoritative record carries boot " + record.reference.boot.value() +
                    " but the agent reports " + agent.boot().value());
    expect_true(record.reference.publisher.has_value() &&
                    *record.reference.publisher == publisher,
                "the authoritative record is not owned by publisher " + publisher.value());
    expect_true(state.lifecycle == ClusterLifecycle::Ready,
                "lifecycle after publish=" + std::string(to_string(state.lifecycle)));
  }
  expect_true(coordinator.invariants().ok(),
              "invariants after publish: " + coordinator.invariants().describe());

  // The boot identity taken before the supersede below.
  const RackAgentBootId old_boot = agent.boot();

  // --- heartbeat: accepted and no commit ----------------------------------
  const ClusterGeneration before_heartbeat = coordinator.state_copy().generation;
  const std::uint64_t commits_before_heartbeat = coordinator.stats().commits;
  const MutationResult heartbeat = agent.heartbeat();
  expect_accepted(heartbeat, "RackAgent::heartbeat()");
  const ClusterState after_heartbeat = coordinator.state_copy();
  expect_true(after_heartbeat.generation == before_heartbeat,
              "heartbeat changed the cluster generation from " + before_heartbeat.str() + " to " +
                  after_heartbeat.generation.str());
  const std::uint64_t commits_after_heartbeat = coordinator.stats().commits;
  expect_true(commits_after_heartbeat == commits_before_heartbeat,
              "heartbeat committed state: commits went from " +
                  std::to_string(commits_before_heartbeat) + " to " +
                  std::to_string(commits_after_heartbeat));

  // --- snapshot round trip -------------------------------------------------
  const SnapshotResponseMessage snapshot_response = agent.request_snapshot();
  expect_true(snapshot_response.accepted,
              "request_snapshot was refused: reason=" +
                  std::string(to_string(snapshot_response.reason)) + " detail=" +
                  snapshot_response.detail);
  const std::string coordinator_digest = coordinator.snapshot().semantic_digest();
  expect_true(snapshot_response.view.semantic_digest == coordinator_digest,
              "request_snapshot returned digest " + snapshot_response.view.semantic_digest +
                  " but the coordinator snapshot digest is " + coordinator_digest);
  expect_true(agent.last_snapshot_digest().has_value(),
              "the agent recorded no snapshot digest");
  expect_true(agent.last_snapshot_digest().value() == coordinator_digest,
              "the agent's recorded digest " + agent.last_snapshot_digest().value() +
                  " differs from the coordinator digest " + coordinator_digest);
  expect_true(snapshot_response.view.rack_count == 1,
              "the snapshot view reports " + std::to_string(snapshot_response.view.rack_count) +
                  " rack(s)");

  const ValidateSnapshotResponseMessage validation =
      agent.validate_snapshot(snapshot_response.view);
  expect_true(validation.validation.current,
              "validate_snapshot did not report current: " + validation.validation.describe());
  expect_true(validation.validation.consumable,
              "validate_snapshot did not report consumable: " + validation.validation.describe());
  expect_true(validation.validation.reasons.empty(),
              "validate_snapshot reported stale reasons: " + validation.validation.describe());

  // --- supersede -----------------------------------------------------------
  const ClusterGeneration before_supersede = coordinator.state_copy().generation;
  const RackGeneration superseded = RackGeneration::from_raw(2);
  const MutationResult supersede_result = agent.supersede(superseded);
  expect_accepted(supersede_result, "RackAgent::supersede()");
  expect_true(agent.generation() == superseded,
              "agent generation after supersede=" + agent.generation().str());
  {
    const ClusterState state = coordinator.state_copy();
    const auto it = state.racks.find(rack);
    expect_true(it != state.racks.end(), "rack/" + rack.value() + " is absent after supersede");
    expect_true(it->second.reference.generation == superseded,
                "state generation after supersede=" + it->second.reference.generation.str() +
                    " expected " + superseded.str());
    expect_true(it->second.authoritative_current,
                "the superseded rack is not authoritative current");
    expect_true(it->second.reference.currentness == RackCurrentness::Current,
                "currentness after supersede=" +
                    std::string(to_string(it->second.reference.currentness)));
    expect_true(state.generation > before_supersede,
                "the cluster generation did not advance across the supersede");
    expect_true(!state.is_boot_fenced(old_boot),
                "the live agent's boot was fenced while it is still connected");
  }
  expect_true(coordinator.invariants().ok(),
              "invariants after supersede: " + coordinator.invariants().describe());

  // --- revalidate ----------------------------------------------------------
  const MutationResult revalidated = agent.revalidate();
  expect_accepted(revalidated, "RackAgent::revalidate()");
  {
    const ClusterState state = coordinator.state_copy();
    const auto it = state.racks.find(rack);
    expect_true(it != state.racks.end(), "rack/" + rack.value() + " is absent after revalidate");
    expect_true(it->second.reference.evidence.is_current(),
                "evidence is not current after revalidate (freshness=" +
                    std::string(to_string(it->second.reference.evidence.freshness)) + ")");
  }

  // --- stop ----------------------------------------------------------------
  agent.stop();
  expect_true(agent.status().state == RackAgentState::Stopped,
              "agent state after stop=" + std::string(to_string(agent.status().state)));
  const MutationResult after_stop = agent.publish();
  expect_true(after_stop.outcome == MutationOutcome::Rejected,
              "publish after stop() returned " + describe_result(after_stop));
  expect_true(!after_stop.error.ok(),
              "publish after stop() returned no structured error: " + describe_result(after_stop));
  const ProtocolStatus after_stop_transport = agent.status().transport;
  expect_true(after_stop_transport == ProtocolStatus::NotConnected ||
                  after_stop_transport == ProtocolStatus::IoError ||
                  after_stop_transport == ProtocolStatus::NotRunning,
              "transport status after a publish following stop() is " +
                  std::string(to_string(after_stop_transport)) +
                  ", expected NotConnected, IoError or NotRunning");
  agent.stop();

  // --- the stopped incarnation is fenced -----------------------------------
  bool fenced = false;
  for (std::size_t attempt = 0; attempt < kPollAttempts && !fenced; ++attempt) {
    fenced = coordinator.state_copy().is_boot_fenced(old_boot);
    if (!fenced) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMillis));
    }
  }
  expect_true(fenced, "boot " + old_boot.value() +
                          " was not fenced after the agent disconnected");

  // --- a second agent reusing the old boot identity is refused -------------
  RackAgentConfig second_config = agent_config(cluster, rack, publisher, superseded, port);
  second_config.boot = old_boot;
  RackAgent reuse(second_config);
  const bool reuse_connected = reuse.connect();
  expect_true(reuse_connected,
              "the second agent could not connect: state=" +
                  std::string(to_string(reuse.status().state)) + " transport=" +
                  std::string(to_string(reuse.status().transport)) + " rejection=" +
                  std::string(to_string(reuse.status().rejection)) + " detail=" +
                  reuse.status().detail);
  const MutationResult reuse_publish = reuse.publish();
  const RejectionReason reuse_reason = reuse_publish.reason;
  expect_true(reuse_publish.outcome == MutationOutcome::Rejected &&
                  (reuse_reason == RejectionReason::StaleRackBoot ||
                   reuse_reason == RejectionReason::NotAuthorized),
              "a publication from the fenced boot " + old_boot.value() +
                  " was not refused with REJECT_STALE_RACK_BOOT or REJECT_NOT_AUTHORIZED: " +
                  describe_result(reuse_publish) + " | agent status rejection=" +
                  std::string(to_string(reuse.status().rejection)) + " detail=" +
                  reuse.status().detail);
  reuse.stop();

  coordinator.stop();
}

int main() { return cf_test::run("test_rack_agent"); }
