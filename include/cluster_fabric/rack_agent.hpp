// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Rack-side publisher.
//
// A rack agent is an independent operating-system process that represents one
// rack under one authoritative RackGeneration and one process-incarnation
// identity. Every mutation it sends is bound to that identity, to the cluster
// and coordinator epochs it believes current, and to a monotonic publication
// counter. When the process dies the coordinator fences the incarnation and
// the dynamic evidence it owned becomes revalidation-required.

#ifndef CLUSTER_FABRIC_RACK_AGENT_HPP
#define CLUSTER_FABRIC_RACK_AGENT_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cluster_fabric/evidence.hpp"
#include "cluster_fabric/generation.hpp"
#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/lifecycle.hpp"
#include "cluster_fabric/mutation.hpp"
#include "cluster_fabric/protocol.hpp"
#include "cluster_fabric/rack_reference.hpp"
#include "cluster_fabric/snapshot.hpp"

namespace cluster_fabric {

struct RackAgentConfig {
  ClusterId cluster;
  RackId rack;
  RackGeneration generation;
  RackPublisherId publisher;
  /// Fresh process-incarnation identity. Generated when absent.
  std::optional<RackAgentBootId> boot;

  std::string coordinator_host = "127.0.0.1";
  std::uint16_t coordinator_port = 0;

  /// Time-to-live attached to published evidence.
  std::int64_t publication_ttl_millis = 30'000;
  std::string origin_label = "cluster-fabric-rack-agent";
  RackLifecycleState rack_lifecycle = RackLifecycleState::Ready;
  HealthState health = HealthState::Healthy;
  Reachability reachability = Reachability::Reachable;
  RackCompositionSummary composition;
  std::vector<RackEndpoint> endpoints;
  std::vector<RackFailureDomainHint> failure_domain_hints;

  /// Optional cluster-level topology the agent is authorized to publish.
  std::vector<InterRackLink> links;
  std::vector<FailureDomain> failure_domains;
  std::vector<PlacementDomain> placement_domains;
};

enum class RackAgentState : std::uint8_t {
  Idle = 0,
  Connecting,
  Handshaking,
  Registering,
  Publishing,
  Ready,
  Failed,
  Stopped,
};

[[nodiscard]] std::string_view to_string(RackAgentState value) noexcept;

struct RackAgentStatus {
  RackAgentState state = RackAgentState::Idle;
  ProtocolStatus transport = ProtocolStatus::Ok;
  RejectionReason rejection = RejectionReason::None;
  std::string detail;
  CoordinatorEpoch coordinator_epoch;
  ClusterEpoch cluster_epoch;
  TopologyEpoch topology_epoch;
  TopologyGeneration topology_generation;
  PublicationGeneration publication;
  bool registered = false;
  bool fenced = false;
  bool connected = false;
};

class RackAgent {
 public:
  explicit RackAgent(RackAgentConfig config, const Clock* clock = nullptr);
  ~RackAgent();

  RackAgent(const RackAgent&) = delete;
  RackAgent& operator=(const RackAgent&) = delete;

  /// Transport connect, HELLO/HELLO_ACK handshake and REGISTER. Returns false
  /// and records a typed status on failure.
  bool connect();
  /// Publishes the current rack reference and any configured topology. Returns
  /// a typed mutation result.
  [[nodiscard]] MutationResult publish();
  /// Advances to p new_generation under current authority and publishes.
  [[nodiscard]] MutationResult supersede(RackGeneration new_generation);
  /// Republishes dynamic evidence after a coordinator restart.
  [[nodiscard]] MutationResult revalidate();
  [[nodiscard]] MutationResult heartbeat();

  [[nodiscard]] SnapshotResponseMessage request_snapshot();
  [[nodiscard]] ValidateSnapshotResponseMessage validate_snapshot(const SnapshotView& view);

  /// Closes the transport and releases process-local resources. Idempotent.
  void stop();

  [[nodiscard]] const RackAgentBootId& boot() const noexcept;
  [[nodiscard]] const RackAgentStatus& status() const noexcept;
  [[nodiscard]] RackGeneration generation() const noexcept;
  [[nodiscard]] std::optional<std::string> last_snapshot_digest() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_RACK_AGENT_HPP
