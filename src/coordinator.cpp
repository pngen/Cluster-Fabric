// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Cluster coordinator lifecycle, public API and deterministic explanations.

#include "cluster_fabric/coordinator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cluster_fabric/limits.hpp"
#include "cluster_fabric/version.hpp"
#include "detail/coordinator_impl.hpp"

namespace cluster_fabric {

CommitHook::~CommitHook() = default;

ClusterCoordinator::Impl::Impl(CoordinatorConfig coordinator_config, PersistenceStore* persistence,
                               const Clock* process_clock)
    : config(std::move(coordinator_config)), store(persistence), clock(process_clock) {
  canonical.id = config.cluster;
  canonical.readiness_contract = config.readiness_contract;
  canonical.coordinator_epoch = CoordinatorEpoch::from_raw(1);
}

Timestamp ClusterCoordinator::Impl::now() const {
  const Clock& source = clock != nullptr ? *clock : default_clock();
  return source.now();
}

ClusterState ClusterCoordinator::Impl::state_copy() {
  std::lock_guard<std::mutex> lock(state_mutex);
  return canonical;
}

ClusterSnapshot ClusterCoordinator::Impl::make_snapshot() {
  std::lock_guard<std::mutex> lock(state_mutex);
  ++snapshots_built;
  return ClusterSnapshot::capture(canonical);
}

MutationResult ClusterCoordinator::Impl::submit(const MutationRequest& request) {
  auto slot = std::make_shared<detail::RequestSlot>();
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (!running.load()) {
      // No commit thread can answer this request. Distinguish "never started"
      // from "already stopped" instead of waiting forever for a result.
      if (stopping) {
        return MutationResult::rejected(RejectionReason::ShuttingDown, ErrorStage::Commit,
                                        request.cluster.value(),
                                        "coordinator has been stopped and accepts no new mutations",
                                        "shutting_down");
      }
      return MutationResult::rejected(RejectionReason::NotReady, ErrorStage::Commit,
                                      request.cluster.value(),
                                      "coordinator is not running: call start() before submitting "
                                      "mutations",
                                      "not_running");
    }
    if (stopping) {
      return MutationResult::rejected(RejectionReason::ShuttingDown, ErrorStage::Commit,
                                      request.cluster.value(),
                                      "coordinator is shutting down and accepts no new mutations",
                                      "shutting_down");
    }
    if (queue.size() >= kMaxPendingRequests) {
      return MutationResult::rejected(RejectionReason::LimitExceeded, ErrorStage::Commit,
                                      request.cluster.value(),
                                      "pending request queue is full", "queue_full");
    }
    detail::QueueItem item;
    item.kind = detail::QueueItem::Kind::Mutation;
    item.request = request;
    item.slot = slot;
    queue.push_back(std::move(item));
  }
  queue_condition.notify_one();

  std::unique_lock<std::mutex> lock(slot->mutex);
  slot->condition.wait(lock, [&slot]() { return slot->done; });
  return slot->result;
}

bool ClusterCoordinator::Impl::load_durable_state() {
  recovery = RecoveryReport{};
  recovery.attempted = store != nullptr;
  const Timestamp started = now();
  CoordinatorEpoch previous = CoordinatorEpoch::from_raw(1);
  PersistedState persisted;
  bool have_persisted = false;

  if (store != nullptr) {
    PersistenceStore::LoadOutcome outcome = store->load();
    recovery.status = outcome.status;
    if (outcome.ok()) {
      have_persisted = true;
      persisted = std::move(*outcome.state);
      previous = persisted.last_coordinator_epoch;
      recovery.loaded = true;
      recovery.notes.push_back("durable state loaded: format version " +
                               std::to_string(kPersistenceFormatVersion));
    } else if (outcome.status == PersistenceStatus::MissingFile) {
      recovery.notes.push_back("no durable container found; starting a fresh cluster");
    } else {
      recovery.notes.push_back("durable container rejected: " +
                               std::string(to_string(outcome.status)));
      if (!outcome.error.message.empty()) {
        recovery.notes.push_back(outcome.error.message);
      }
      recovery.status = outcome.status;
    }
  }

  CoordinatorEpoch next = previous.known() ? previous.next().value_or(previous) : CoordinatorEpoch::from_raw(1);
  if (!next.known()) {
    next = CoordinatorEpoch::from_raw(1);
  }
  if (have_persisted) {
    recovery.previous_coordinator_epoch = previous;
    recovery.current_coordinator_epoch = next;
    recovery.coordinator_epoch_advanced = next != previous;
    canonical = detail::from_persisted_state(persisted, next, started, recovery);
    canonical.coordinator_epoch = next;
    canonical.readiness_contract = persisted.readiness_contract;
  } else {
    canonical = ClusterState{};
    canonical.id = config.cluster;
    canonical.readiness_contract = config.readiness_contract;
    canonical.coordinator_epoch = next;
    recovery.current_coordinator_epoch = next;
  }
  indexes_valid = false;
  ++state_version;

  if (!recovery.loaded) {
    recovery.revalidation_required = false;
  }
  std::sort(recovery.notes.begin(), recovery.notes.end());
  return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ClusterCoordinator::ClusterCoordinator(CoordinatorConfig config, PersistenceStore* store,
                                       const Clock* clock)
    : impl_(std::make_unique<Impl>(std::move(config), store, clock)) {}

ClusterCoordinator::~ClusterCoordinator() {
  if (impl_ != nullptr) {
    impl_->stop();
  }
}

CoordinatorStartOutcome ClusterCoordinator::start() {
  if (impl_ == nullptr) {
    CoordinatorStartOutcome outcome;
    outcome.status = ProtocolStatus::NotRunning;
    outcome.error = StructuredError::make(ErrorCategory::Internal, ErrorStage::Transport,
                                          "no_implementation",
                                          "coordinator implementation is missing");
    return outcome;
  }
  if (impl_->commit_thread.joinable() || impl_->accepting.load()) {
    CoordinatorStartOutcome outcome;
    outcome.status = ProtocolStatus::AlreadyRunning;
    outcome.error = StructuredError::make(ErrorCategory::Internal, ErrorStage::Transport,
                                          "already_running", "coordinator is already running");
    return outcome;
  }
  if (!detail::socket_library_init()) {
    CoordinatorStartOutcome outcome;
    outcome.status = ProtocolStatus::IoError;
    outcome.error = StructuredError::make(ErrorCategory::Transport, ErrorStage::Transport,
                                          "socket_init_failed", detail::socket_error_text());
    return outcome;
  }
  (void)impl_->load_durable_state();

  {
    std::lock_guard<std::mutex> lock(impl_->queue_mutex);
    impl_->stopping = false;
  }
  impl_->commit_thread = std::thread([this]() { impl_->run_commit(); });
  impl_->running.store(true);

  const detail::socket_handle listener = detail::open_listener(impl_->config.bind_address,
                                                               impl_->config.port,
                                                               impl_->config.max_sessions);
  if (listener == detail::kInvalidSocket) {
    {
      std::lock_guard<std::mutex> lock(impl_->queue_mutex);
      impl_->stopping = true;
      detail::QueueItem item;
      item.kind = detail::QueueItem::Kind::Stop;
      impl_->queue.push_back(std::move(item));
    }
    impl_->queue_condition.notify_all();
    if (impl_->commit_thread.joinable()) {
      impl_->commit_thread.join();
    }
    CoordinatorStartOutcome outcome;
    outcome.status = ProtocolStatus::IoError;
    outcome.error = StructuredError::make(ErrorCategory::Transport, ErrorStage::Transport,
                                          "listen_failed", detail::socket_error_text());
    return outcome;
  }
  impl_->listener = listener;
  impl_->bound_port = detail::listener_port(listener);
  impl_->accepting = true;
  impl_->accept_thread = std::thread([this]() { impl_->run_accept(); });

  CoordinatorStartOutcome outcome;
  outcome.ok = true;
  outcome.status = ProtocolStatus::Ok;
  outcome.port = impl_->bound_port;
  return outcome;
}

void ClusterCoordinator::stop() {
  if (impl_ == nullptr) {
    return;
  }
  impl_->stop();
}

bool ClusterCoordinator::running() const noexcept {
  return impl_ != nullptr && impl_->accepting.load();
}

std::uint16_t ClusterCoordinator::port() const noexcept {
  return impl_ != nullptr ? impl_->bound_port : 0;
}

const ClusterId& ClusterCoordinator::cluster() const noexcept { return impl_->config.cluster; }

MutationResult ClusterCoordinator::submit(const MutationRequest& request) {
  return impl_->submit(request);
}

bool ClusterCoordinator::fence_boot(const RackAgentBootId& boot, std::string_view reason) {
  if (impl_ == nullptr) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->queue_mutex);
    if (impl_->stopping) {
      return false;
    }
    detail::QueueItem item;
    item.kind = detail::QueueItem::Kind::Fence;
    item.fence_boot = boot;
    item.fence_reason = std::string(reason);
    impl_->queue.push_back(std::move(item));
  }
  impl_->queue_condition.notify_one();
  return true;
}

ClusterSnapshot ClusterCoordinator::snapshot() const { return impl_->make_snapshot(); }

SnapshotValidation ClusterCoordinator::validate(const ClusterSnapshot& snapshot) const {
  ++impl_->validation_requests;
  return snapshot.validate(impl_->state_copy());
}

SnapshotValidation ClusterCoordinator::validate(const SnapshotView& view) const {
  ++impl_->validation_requests;
  return impl_->validate_view(view);
}

ClusterState ClusterCoordinator::state_copy() const { return impl_->state_copy(); }

ReadinessEvaluation ClusterCoordinator::readiness() const {
  return impl_->with_state(
      [](const ClusterState& state, const ClusterIndexes&) { return evaluate_readiness(state); });
}

InvariantReport ClusterCoordinator::invariants() const {
  return impl_->with_state([](const ClusterState& state, const ClusterIndexes& indexes) {
    return check_invariants(state, &indexes);
  });
}

CoordinatorStats ClusterCoordinator::stats() const {
  CoordinatorStats stats;
  stats.mutations_accepted = impl_->mutations_accepted.load();
  stats.mutations_no_change = impl_->mutations_no_change.load();
  stats.mutations_rejected = impl_->mutations_rejected.load();
  stats.sessions_opened = impl_->sessions_opened.load();
  stats.sessions_closed = impl_->sessions_closed.load();
  stats.frames_in = impl_->frames_in.load();
  stats.frames_out = impl_->frames_out.load();
  stats.bytes_in = impl_->bytes_in.load();
  stats.bytes_out = impl_->bytes_out.load();
  stats.commits = impl_->commits.load();
  stats.persistence_saves = impl_->persistence_saves.load();
  stats.persistence_failures = impl_->persistence_failures.load();
  stats.fence_events = impl_->fence_events.load();
  stats.stale_replays_rejected = impl_->stale_replays_rejected.load();
  stats.snapshots_built = impl_->snapshots_built.load();
  stats.validation_requests = impl_->validation_requests.load();
  return stats;
}

const RecoveryReport& ClusterCoordinator::recovery() const { return impl_->recovery; }

void ClusterCoordinator::set_commit_hook(CommitHook* hook) noexcept { impl_->hook.store(hook); }

Explanation ClusterCoordinator::explain_lifecycle() const {
  return impl_->with_state([](const ClusterState& state, const ClusterIndexes&) {
    const ReadinessEvaluation evaluation = evaluate_readiness(state);
    Explanation explanation =
        Explanation::make("cluster_lifecycle", "cluster/" + state.id.value(),
                          "lifecycle is " + std::string(to_string(evaluation.lifecycle)) +
                              (evaluation.satisfied ? " (readiness contract satisfied)"
                                                    : " (readiness contract not satisfied)"));
    for (const ReadinessBlocker& blocker : evaluation.blockers) {
      explanation.add(blocker.code, blocker.subject, blocker.detail);
    }
    explanation.sort_factors();
    explanation.bound_factors();
    return explanation;
  });
}

Explanation ClusterCoordinator::explain_rack(const RackId& rack) const {
  return impl_->with_state([&rack](const ClusterState& state, const ClusterIndexes& indexes) {
    Explanation explanation = Explanation::make("rack_status", "rack/" + rack.value(), "");
    const auto it = state.racks.find(rack);
    if (it == state.racks.end()) {
      explanation.summary = "rack is not a member of this cluster";
      explanation.add("unknown_rack", "rack/" + rack.value(),
                      "no canonical record exists for this identity");
      explanation.sort_factors();
      return explanation;
    }
    const RackRecord& record = it->second;
    explanation.summary = "membership " + std::string(to_string(record.membership)) +
                          " currentness " +
                          std::string(to_string(record.reference.currentness)) +
                          (record.authoritative_current ? " authoritative" : " not authoritative");
    explanation.add("rack_generation", "rack/" + rack.value(),
                    "generation " + record.reference.generation.str());
    explanation.add("membership", "rack/" + rack.value(),
                    std::string(to_string(record.membership)));
    explanation.add("evidence", "rack/" + rack.value(),
                    "provenance " + std::string(to_string(record.reference.evidence.provenance)) +
                        " freshness " + std::string(to_string(record.reference.evidence.freshness)));
    if (!record.authoritative_current && !record.non_authoritative_reason.empty()) {
      explanation.add("not_authoritative", "rack/" + rack.value(),
                      record.non_authoritative_reason);
    }
    if (state.is_boot_fenced(record.reference.boot)) {
      explanation.add("boot_fenced", "rack/" + rack.value(),
                      "process incarnation " + record.reference.boot.value() +
                          " is permanently fenced");
    }
    if (state.is_rack_retired(rack)) {
      explanation.add("retired", "rack/" + rack.value(), "rack identity is retired");
    }
    for (const PlacementDomainId& domain : indexes.placement_domains_of(rack)) {
      explanation.add("placement_domain", "placement_domain/" + domain.value(),
                      "rack is a member");
    }
    for (const FailureDomainId& domain : indexes.failure_domains_of(rack)) {
      explanation.add("failure_domain", "failure_domain/" + domain.value(),
                      "rack is a member");
    }
    explanation.sort_factors();
    explanation.bound_factors();
    return explanation;
  });
}

Explanation ClusterCoordinator::explain_topology_currentness(TopologyEpoch epoch,
                                                             TopologyGeneration generation) const {
  return impl_->with_state(
      [epoch, generation](const ClusterState& state, const ClusterIndexes&) {
        const bool epoch_current = epoch == state.topology_epoch;
        const bool generation_current = generation == state.topology_generation;
        Explanation explanation = Explanation::make(
            epoch_current && generation_current ? "topology_current" : "topology_stale",
            "topology", epoch_current && generation_current
                            ? "topology epoch and generation are current"
                            : "topology epoch or generation has been superseded");
        explanation.add("authoritative_epoch", "topology", state.topology_epoch.str());
        explanation.add("authoritative_generation", "topology", state.topology_generation.str());
        explanation.add("supplied_epoch", "topology", epoch.str());
        explanation.add("supplied_generation", "topology", generation.str());
        if (!epoch_current) {
          explanation.add("stale_topology_epoch", "topology",
                          "supplied epoch is not the authoritative epoch");
        }
        if (!generation_current) {
          explanation.add("stale_topology_generation", "topology",
                          "supplied generation is not the authoritative generation");
        }
        explanation.sort_factors();
        explanation.bound_factors();
        return explanation;
      });
}

Explanation ClusterCoordinator::explain_snapshot_staleness(const SnapshotView& view) const {
  const SnapshotValidation validation = impl_->validate_view(view);
  Explanation explanation = validation.explanation;
  if (explanation.code.empty()) {
    explanation = Explanation::make(validation.current ? "snapshot_current" : "snapshot_stale",
                                    "snapshot", validation.describe());
  }
  return explanation;
}

Explanation ClusterCoordinator::explain_failure_domain(const RackId& lhs, const RackId& rhs,
                                                       FailureDomainClass klass) const {
  return impl_->with_state([&lhs, &rhs, klass](const ClusterState& state,
                                               const ClusterIndexes& indexes) {
    const DomainIndependence independence =
        failure_domain_independence(state, indexes, lhs, rhs, klass);
    Explanation explanation = Explanation::make(
        "failure_domain_independence", "rack/" + lhs.value() + "+" + rhs.value(),
        "racks are " + std::string(to_string(independence)) + " for failure domain class " +
            std::string(to_string(klass)));
    explanation.add("domain_class", "failure_domain_class",
                    std::string(to_string(klass)));
    for (const FailureDomainId& domain : indexes.failure_domains_of(lhs)) {
      const auto it = state.failure_domains.find(domain);
      if (it == state.failure_domains.end() || it->second.klass != klass) {
        continue;
      }
      const bool shared = std::binary_search(it->second.racks.begin(), it->second.racks.end(), rhs);
      explanation.add(shared ? "shared_domain" : "lhs_domain", "failure_domain/" + domain.value(),
                      "evidence " +
                          std::string(to_string(it->second.header.evidence.provenance)) + "/" +
                          std::string(to_string(it->second.header.evidence.freshness)));
    }
    for (const FailureDomainId& domain : indexes.failure_domains_of(rhs)) {
      const auto it = state.failure_domains.find(domain);
      if (it == state.failure_domains.end() || it->second.klass != klass) {
        continue;
      }
      const bool shared = std::binary_search(it->second.racks.begin(), it->second.racks.end(), lhs);
      if (shared) {
        continue;
      }
      explanation.add("rhs_domain", "failure_domain/" + domain.value(),
                      "evidence " +
                          std::string(to_string(it->second.header.evidence.provenance)) + "/" +
                          std::string(to_string(it->second.header.evidence.freshness)));
    }
    if (independence == DomainIndependence::Unknown) {
      explanation.add("insufficient_current_evidence", "failure_domain_class",
                      "no current domain of this class covers both racks, so independence "
                      "cannot be asserted");
    }
    explanation.sort_factors();
    explanation.bound_factors();
    return explanation;
  });
}

Explanation ClusterCoordinator::explain_reachability(const RackId& source,
                                                     const RackId& destination) const {
  return impl_->with_state([&source, &destination](const ClusterState& state,
                                                   const ClusterIndexes&) {
    const Reachability reachability = reachability_between(state, source, destination);
    Explanation explanation =
        Explanation::make("reachability", "rack/" + source.value() + "->" + destination.value(),
                          "reachability is " + std::string(to_string(reachability)));
    for (const auto& entry : state.links) {
      const InterRackLink& link = entry.second;
      const bool forward = (link.source == source && link.destination == destination) ||
                           (link.source == destination && link.destination == source &&
                            link.direction == LinkDirection::Bidirectional);
      if (!forward) {
        continue;
      }
      explanation.add("link", "link/" + entry.first.value(),
                      "reachability " + std::string(to_string(link.reachability)) +
                          " evidence " +
                          std::string(to_string(link.header.evidence.provenance)) + "/" +
                          std::string(to_string(link.header.evidence.freshness)));
    }
    if (reachability == Reachability::Unknown) {
      explanation.add("no_current_link_evidence", "link",
                      "no link carries current evidence for this rack pair; UNKNOWN is not "
                      "evidence of connectivity");
    }
    explanation.sort_factors();
    explanation.bound_factors();
    return explanation;
  });
}

}  // namespace cluster_fabric
