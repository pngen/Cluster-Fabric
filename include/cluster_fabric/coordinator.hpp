// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The cluster coordinator: the single authority for one cluster composition.
//
// The coordinator is a single-writer, multi-reader service. Mutations are
// serialized through one commit path that owns canonical state; readers always
// observe a published, immutable state object. Network I/O is never performed
// while holding the canonical state, and no callback is invoked under a lock
// that it could reacquire.

#ifndef CLUSTER_FABRIC_COORDINATOR_HPP
#define CLUSTER_FABRIC_COORDINATOR_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "cluster_fabric/cluster_state.hpp"
#include "cluster_fabric/error.hpp"
#include "cluster_fabric/explanation.hpp"
#include "cluster_fabric/invariant.hpp"
#include "cluster_fabric/lifecycle.hpp"
#include "cluster_fabric/mutation.hpp"
#include "cluster_fabric/persistence.hpp"
#include "cluster_fabric/protocol.hpp"
#include "cluster_fabric/snapshot.hpp"

namespace cluster_fabric {

struct CoordinatorConfig {
  ClusterId cluster;
  ReadinessContract readiness_contract;

  /// Listener address. Loopback by default: this build is validated as a
  /// single-host multiprocess reference deployment.
  std::string bind_address = "127.0.0.1";
  /// 0 selects an ephemeral port; the bound port is reported by start().
  std::uint16_t port = 0;

  std::size_t max_sessions = kMaxSessions;
  std::uint32_t max_frame_payload = kMaxFramePayloadBytes;
  std::size_t max_racks = kMaxRacksPerCluster;
  std::size_t max_links = kMaxInterRackLinks;
  std::size_t max_domains_per_class = kMaxDomainsPerClass;
  std::size_t max_constraints = kMaxConstraints;
  /// Default time-to-live applied to evidence that does not declare one.
  std::int64_t default_ttl_millis = 30'000;
  /// Persist durable state after every accepted commit.
  bool persist_on_commit = true;
  /// Require a rack publisher to register before it may publish rack evidence.
  bool require_registration = true;
  /// When true, a rack whose publishing process disconnects immediately
  /// requires revalidation. When false the rack is marked DEGRADED and its
  /// evidence is marked stale but the cluster may remain READY if the
  /// readiness contract permits it.
  bool fence_on_disconnect = true;
};

struct CoordinatorStats {
  std::uint64_t mutations_accepted = 0;
  std::uint64_t mutations_no_change = 0;
  std::uint64_t mutations_rejected = 0;
  std::uint64_t sessions_opened = 0;
  std::uint64_t sessions_closed = 0;
  std::uint64_t frames_in = 0;
  std::uint64_t frames_out = 0;
  std::uint64_t bytes_in = 0;
  std::uint64_t bytes_out = 0;
  std::uint64_t commits = 0;
  std::uint64_t persistence_saves = 0;
  std::uint64_t persistence_failures = 0;
  std::uint64_t fence_events = 0;
  std::uint64_t stale_replays_rejected = 0;
  std::uint64_t snapshots_built = 0;
  std::uint64_t validation_requests = 0;
};

struct CoordinatorStartOutcome {
  bool ok = false;
  ProtocolStatus status = ProtocolStatus::Ok;
  std::uint16_t port = 0;
  StructuredError error;

  [[nodiscard]] bool succeeded() const noexcept { return ok; }
};

/// What happened when durable state was loaded at start.
struct RecoveryReport {
  bool attempted = false;
  bool loaded = false;
  PersistenceStatus status = PersistenceStatus::Ok;
  std::size_t racks_recovered = 0;
  std::size_t domains_recovered = 0;
  std::size_t links_recovered = 0;
  std::size_t retired_recovered = 0;
  std::size_t fenced_recovered = 0;
  bool coordinator_epoch_advanced = false;
  CoordinatorEpoch previous_coordinator_epoch;
  CoordinatorEpoch current_coordinator_epoch;
  /// True when recovered dynamic evidence was cleared and now requires
  /// revalidation.
  bool revalidation_required = false;
  /// Deterministic notes, sorted.
  std::vector<std::string> notes;
};

/// Optional observer used by deterministic race tests. Hooks are invoked from
/// the commit thread and must not call back into the coordinator.
class CommitHook {
 public:
  CommitHook() = default;
  CommitHook(const CommitHook&) = delete;
  CommitHook& operator=(const CommitHook&) = delete;
  virtual ~CommitHook();

  /// Invoked immediately after a mutation has been validated and before the
  /// candidate state is constructed.
  virtual void before_candidate(MutationKind kind) { (void)kind; }
  /// Invoked immediately before the candidate is committed.
  virtual void before_commit(MutationKind kind) { (void)kind; }
  /// Invoked immediately after the new state has been published. No lock is
  /// held here.
  virtual void after_publish(MutationKind kind) { (void)kind; }
};

class ClusterCoordinator {
 public:
  /// p store and p clock are borrowed and must outlive the coordinator. A
  /// null store means durable state is not persisted. A null clock means the
  /// process wall clock is used.
  explicit ClusterCoordinator(CoordinatorConfig config, PersistenceStore* store = nullptr,
                              const Clock* clock = nullptr);
  ~ClusterCoordinator();

  ClusterCoordinator(const ClusterCoordinator&) = delete;
  ClusterCoordinator& operator=(const ClusterCoordinator&) = delete;

  /// Opens the listener and loads durable state. Idempotent failure: a second
  /// call while running returns AlreadyRunning.
  [[nodiscard]] CoordinatorStartOutcome start();
  /// Stops accepting sessions, drains and rejects pending work deterministically,
  /// joins every worker without holding a lock they need, and closes sockets.
  void stop();
  /// True while the listener and commit path are running.
  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] const ClusterId& cluster() const noexcept;

  /// Applies one mutation through the transactional commit path.
  [[nodiscard]] MutationResult submit(const MutationRequest& request);

  /// Permanently fences a process incarnation. Used by the session layer on
  /// disconnect and by tests.
  bool fence_boot(const RackAgentBootId& boot, std::string_view reason);

  [[nodiscard]] ClusterSnapshot snapshot() const;
  [[nodiscard]] SnapshotValidation validate(const ClusterSnapshot& snapshot) const;
  [[nodiscard]] SnapshotValidation validate(const SnapshotView& view) const;
  [[nodiscard]] ClusterState state_copy() const;
  [[nodiscard]] ReadinessEvaluation readiness() const;
  [[nodiscard]] InvariantReport invariants() const;
  [[nodiscard]] CoordinatorStats stats() const;
  [[nodiscard]] const RecoveryReport& recovery() const;

  // Deterministic explanations.
  [[nodiscard]] Explanation explain_lifecycle() const;
  [[nodiscard]] Explanation explain_rack(const RackId& rack) const;
  [[nodiscard]] Explanation explain_topology_currentness(TopologyEpoch epoch,
                                                        TopologyGeneration generation) const;
  [[nodiscard]] Explanation explain_snapshot_staleness(const SnapshotView& view) const;
  [[nodiscard]] Explanation explain_failure_domain(const RackId& lhs, const RackId& rhs,
                                                   FailureDomainClass klass) const;
  [[nodiscard]] Explanation explain_reachability(const RackId& source,
                                                 const RackId& destination) const;

  /// Installs an observer for deterministic race tests. Pass nullptr to clear.
  void set_commit_hook(CommitHook* hook) noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_COORDINATOR_HPP
