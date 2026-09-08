// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Transactional commit path and durable-state projection.
//
// Every mutation follows the same sequence:
//   validate authority -> validate generations -> resolve references ->
//   apply to canonical state under an undo journal -> verify invariants ->
//   persist -> advance generations -> publish.
// A failure at any stage rolls the journal back, so no partial cluster graph
// and no half-created domain can ever become authoritative.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cluster_fabric/limits.hpp"
#include "detail/coordinator_impl.hpp"

namespace cluster_fabric {
namespace detail {

namespace {

/// Everything outside the canonical maps that a mutation may change.
struct ScalarSnapshot {
  ClusterEpoch epoch;
  CoordinatorEpoch coordinator_epoch;
  ClusterGeneration generation;
  MembershipGeneration membership_generation;
  TopologyEpoch topology_epoch;
  TopologyGeneration topology_generation;
  ConnectivityGeneration connectivity_generation;
  HealthGeneration health_generation;
  ConstraintGeneration constraint_generation;
  DomainGenerations domain_generations;
  SnapshotGeneration snapshot_generation;
  PublicationGeneration publication_generation;
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;
  ReadinessContract readiness_contract;
  TopologyEpochRecord topology_record;
  std::vector<FencedAuthority> fenced_authorities;
  std::vector<RetiredIdentity> retired_racks;
  std::vector<RackId> withdrawn_racks;
  Timestamp last_mutation_at = Timestamp::unknown();
  Timestamp declared_at = Timestamp::unknown();
};

[[nodiscard]] ScalarSnapshot capture_scalars(const ClusterState& state) {
  ScalarSnapshot snapshot;
  snapshot.epoch = state.epoch;
  snapshot.coordinator_epoch = state.coordinator_epoch;
  snapshot.generation = state.generation;
  snapshot.membership_generation = state.membership_generation;
  snapshot.topology_epoch = state.topology_epoch;
  snapshot.topology_generation = state.topology_generation;
  snapshot.connectivity_generation = state.connectivity_generation;
  snapshot.health_generation = state.health_generation;
  snapshot.constraint_generation = state.constraint_generation;
  snapshot.domain_generations = state.domain_generations;
  snapshot.snapshot_generation = state.snapshot_generation;
  snapshot.publication_generation = state.publication_generation;
  snapshot.lifecycle = state.lifecycle;
  snapshot.readiness_contract = state.readiness_contract;
  snapshot.topology_record = state.topology_record;
  snapshot.fenced_authorities = state.fenced_authorities;
  snapshot.retired_racks = state.retired_racks;
  snapshot.withdrawn_racks = state.withdrawn_racks;
  snapshot.last_mutation_at = state.last_mutation_at;
  snapshot.declared_at = state.declared_at;
  return snapshot;
}

void restore_scalars(ClusterState& state, const ScalarSnapshot& snapshot) {
  state.epoch = snapshot.epoch;
  state.coordinator_epoch = snapshot.coordinator_epoch;
  state.generation = snapshot.generation;
  state.membership_generation = snapshot.membership_generation;
  state.topology_epoch = snapshot.topology_epoch;
  state.topology_generation = snapshot.topology_generation;
  state.connectivity_generation = snapshot.connectivity_generation;
  state.health_generation = snapshot.health_generation;
  state.constraint_generation = snapshot.constraint_generation;
  state.domain_generations = snapshot.domain_generations;
  state.snapshot_generation = snapshot.snapshot_generation;
  state.publication_generation = snapshot.publication_generation;
  state.lifecycle = snapshot.lifecycle;
  state.readiness_contract = snapshot.readiness_contract;
  state.topology_record = snapshot.topology_record;
  state.fenced_authorities = snapshot.fenced_authorities;
  state.retired_racks = snapshot.retired_racks;
  state.withdrawn_racks = snapshot.withdrawn_racks;
  state.last_mutation_at = snapshot.last_mutation_at;
  state.declared_at = snapshot.declared_at;
}

}  // namespace

void StateJournal::record_scalars() {
  const ScalarSnapshot snapshot = capture_scalars(state_);
  undo_.push_back([this, snapshot]() { restore_scalars(state_, snapshot); });
}

void StateJournal::rollback() {
  for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) {
    (*it)();
  }
  undo_.clear();
}

// ---------------------------------------------------------------------------
// Durable projection
// ---------------------------------------------------------------------------

/// Placeholder authority used only when a canonical record somehow lacks a
/// publisher. Registration and admission always require one.
[[nodiscard]] const RackPublisherId& unknown_publisher() {
  static const RackPublisherId kUnknown = RackPublisherId::parse("cluster-fabric-unknown").value();
  return kUnknown;
}

PersistedState to_persisted_state(const ClusterState& state) {
  PersistedState persisted;
  persisted.id = state.id;
  persisted.epoch = state.epoch;
  persisted.last_coordinator_epoch = state.coordinator_epoch;
  persisted.generation = state.generation;
  persisted.membership_generation = state.membership_generation;
  persisted.topology_epoch = state.topology_epoch;
  persisted.topology_generation = state.topology_generation;
  persisted.connectivity_generation = state.connectivity_generation;
  persisted.health_generation = state.health_generation;
  persisted.constraint_generation = state.constraint_generation;
  persisted.domain_generations = state.domain_generations;
  persisted.snapshot_generation = state.snapshot_generation;
  persisted.publication_generation = state.publication_generation;
  persisted.lifecycle = state.lifecycle;
  persisted.readiness_contract = state.readiness_contract;
  persisted.topology_record = state.topology_record;
  persisted.declared_at = state.declared_at;
  persisted.last_mutation_at = state.last_mutation_at;

  persisted.racks.reserve(state.racks.size());
  for (const auto& entry : state.racks) {
    PersistedState::DurableRack rack;
    rack.rack = entry.first;
    rack.generation = entry.second.reference.generation;
    rack.membership = entry.second.membership;
    rack.publisher = entry.second.reference.publisher.value_or(unknown_publisher());
    rack.last_accepted_publication = entry.second.reference.publication;
    rack.last_boot = entry.second.reference.boot;
    rack.composition = entry.second.reference.composition;
    rack.endpoints = entry.second.reference.endpoints;
    rack.failure_domain_hints = entry.second.reference.failure_domain_hints;
    rack.rack_lifecycle = entry.second.reference.rack_lifecycle;
    rack.membership_generation = entry.second.membership_generation;
    rack.origin_label = entry.second.reference.origin_label;
    rack.declared_at = entry.second.membership_evidence.observed_at;
    persisted.racks.push_back(std::move(rack));
  }

  for (const auto& entry : state.placement_domains) {
    persisted.placement_domains.push_back(entry.second);
  }
  for (const auto& entry : state.capacity_domains) {
    persisted.capacity_domains.push_back(entry.second);
  }
  for (const auto& entry : state.failure_domains) {
    persisted.failure_domains.push_back(entry.second);
  }
  for (const auto& entry : state.network_domains) {
    persisted.network_domains.push_back(entry.second);
  }
  for (const auto& entry : state.storage_domains) {
    persisted.storage_domains.push_back(entry.second);
  }
  for (const auto& entry : state.power_domains) {
    persisted.power_domains.push_back(entry.second);
  }
  for (const auto& entry : state.cooling_domains) {
    persisted.cooling_domains.push_back(entry.second);
  }
  for (const auto& entry : state.link_domains) {
    persisted.link_domains.push_back(entry.second);
  }
  for (const auto& entry : state.links) {
    persisted.links.push_back(entry.second);
  }
  for (const auto& entry : state.constraints) {
    persisted.constraints.push_back(entry.second);
  }
  persisted.retired_racks = state.retired_racks;
  persisted.withdrawn_racks = state.withdrawn_racks;
  persisted.fenced_authorities = state.fenced_authorities;
  return persisted;
}

ClusterState from_persisted_state(const PersistedState& persisted, CoordinatorEpoch coordinator_epoch,
                                 Timestamp now, RecoveryReport& report) {
  ClusterState state;
  state.id = persisted.id;
  state.epoch = persisted.epoch;
  state.coordinator_epoch = coordinator_epoch;
  state.generation = persisted.generation;
  state.membership_generation = persisted.membership_generation;
  state.topology_epoch = persisted.topology_epoch;
  state.topology_generation = persisted.topology_generation;
  state.connectivity_generation = persisted.connectivity_generation;
  state.health_generation = persisted.health_generation;
  state.constraint_generation = persisted.constraint_generation;
  state.domain_generations = persisted.domain_generations;
  state.snapshot_generation = persisted.snapshot_generation;
  state.publication_generation = persisted.publication_generation;
  state.readiness_contract = persisted.readiness_contract;
  state.topology_record = persisted.topology_record;
  state.declared_at = persisted.declared_at;
  state.last_mutation_at = persisted.last_mutation_at;
  state.retired_racks = persisted.retired_racks;
  state.withdrawn_racks = persisted.withdrawn_racks;
  state.fenced_authorities = persisted.fenced_authorities;

  for (const PersistedState::DurableRack& rack : persisted.racks) {
    RackRecord record;
    record.reference.rack = rack.rack;
    record.reference.generation = rack.generation;
    record.reference.rack_lifecycle = rack.rack_lifecycle;
    record.reference.currentness = RackCurrentness::RevalidationRequired;
    record.reference.composition = rack.composition;
    record.reference.endpoints = rack.endpoints;
    record.reference.failure_domain_hints = rack.failure_domain_hints;
    record.reference.evidence.provenance = EvidenceProvenance::Reconstructed;
    record.reference.evidence.observed_at = rack.declared_at;
    record.reference.evidence.freshness = Freshness::RevalidationRequired;
    record.reference.evidence.ttl_millis = 0;
    record.reference.publisher = rack.publisher;
    record.reference.publication = rack.last_accepted_publication;
    record.reference.boot = rack.last_boot;
    record.reference.cluster_epoch = persisted.epoch;
    record.reference.coordinator_epoch = coordinator_epoch;
    record.reference.health = HealthState::Unknown;
    record.reference.origin_label = rack.origin_label;
    record.membership = rack.membership;
    record.membership_generation = rack.membership_generation;
    record.accepted_at_generation = persisted.generation;
    record.membership_evidence.provenance = EvidenceProvenance::Reconstructed;
    record.membership_evidence.observed_at = now;
    record.membership_evidence.freshness = Freshness::RevalidationRequired;
    record.authoritative_current = false;
    record.non_authoritative_reason = "recovered_state_requires_revalidation";
    state.racks.insert_or_assign(rack.rack, std::move(record));
    ++report.racks_recovered;

    const bool already_fenced =
        std::any_of(state.fenced_authorities.begin(), state.fenced_authorities.end(),
                    [&rack](const FencedAuthority& fenced) { return fenced.boot == rack.last_boot; });
    if (!already_fenced) {
      FencedAuthority fenced;
      fenced.boot = rack.last_boot;
      fenced.rack = rack.rack;
      fenced.reason = "coordinator_restart";
      fenced.fenced_at = now;
      state.fenced_authorities.push_back(std::move(fenced));
      ++report.fenced_recovered;
    }
  }
  std::sort(state.fenced_authorities.begin(), state.fenced_authorities.end(),
            [](const FencedAuthority& lhs, const FencedAuthority& rhs) {
              return lhs.boot < rhs.boot;
            });

  for (const PlacementDomain& domain : persisted.placement_domains) {
    PlacementDomain recovered = domain;
    recovered.header.evidence.refresh(now);
    state.placement_domains.insert_or_assign(recovered.id, std::move(recovered));
    ++report.domains_recovered;
  }
  for (const CapacityDomain& domain : persisted.capacity_domains) {
    CapacityDomain recovered = domain;
    recovered.header.evidence.refresh(now);
    state.capacity_domains.insert_or_assign(recovered.id, std::move(recovered));
    ++report.domains_recovered;
  }
  for (const FailureDomain& domain : persisted.failure_domains) {
    FailureDomain recovered = domain;
    recovered.header.evidence.refresh(now);
    state.failure_domains.insert_or_assign(recovered.id, std::move(recovered));
    ++report.domains_recovered;
  }
  for (const NetworkDomain& domain : persisted.network_domains) {
    NetworkDomain recovered = domain;
    recovered.header.evidence.refresh(now);
    state.network_domains.insert_or_assign(recovered.id, std::move(recovered));
    ++report.domains_recovered;
  }
  for (const StorageDomain& domain : persisted.storage_domains) {
    StorageDomain recovered = domain;
    recovered.header.evidence.refresh(now);
    state.storage_domains.insert_or_assign(recovered.id, std::move(recovered));
    ++report.domains_recovered;
  }
  for (const PowerDomain& domain : persisted.power_domains) {
    PowerDomain recovered = domain;
    recovered.header.evidence.refresh(now);
    state.power_domains.insert_or_assign(recovered.id, std::move(recovered));
    ++report.domains_recovered;
  }
  for (const CoolingDomain& domain : persisted.cooling_domains) {
    CoolingDomain recovered = domain;
    recovered.header.evidence.refresh(now);
    state.cooling_domains.insert_or_assign(recovered.id, std::move(recovered));
    ++report.domains_recovered;
  }
  for (const LinkDomain& domain : persisted.link_domains) {
    LinkDomain recovered = domain;
    recovered.header.evidence.refresh(now);
    state.link_domains.insert_or_assign(recovered.id, std::move(recovered));
    ++report.domains_recovered;
  }
  for (const InterRackLink& link : persisted.links) {
    InterRackLink recovered = link;
    recovered.reachability = Reachability::Unknown;
    recovered.health = HealthState::Unknown;
    recovered.header.evidence.mark_revalidation_required();
    state.links.insert_or_assign(recovered.id, std::move(recovered));
    ++report.links_recovered;
  }
  for (const ClusterConstraint& constraint : persisted.constraints) {
    ClusterConstraint recovered = constraint;
    recovered.header.evidence.refresh(now);
    state.constraints.insert_or_assign(recovered.id, std::move(recovered));
  }
  report.retired_recovered = state.retired_racks.size();
  report.revalidation_required = !state.racks.empty();

  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  state.lifecycle = evaluation.lifecycle;
  return state;
}

}  // namespace detail

namespace {

using cluster_fabric::detail::StateJournal;

// ---------------------------------------------------------------------------
// Commit engine
// ---------------------------------------------------------------------------

struct ApplyContext {
  ClusterState& state;
  const CoordinatorConfig& config;
  Timestamp now;
  StateJournal& journal;
  bool changed = false;
};

[[nodiscard]] MutationResult reject_with(RejectionReason reason, ErrorStage stage,
                                         std::string subject, std::string detail,
                                         std::string code = std::string()) {
  return MutationResult::rejected(reason, stage, std::move(subject), std::move(detail),
                                  std::move(code));
}

[[nodiscard]] MutationResult accepted_change() {
  MutationResult result;
  result.outcome = MutationOutcome::Accepted;
  return result;
}

[[nodiscard]] MutationResult accepted_no_change(std::string detail) {
  MutationResult result;
  result.outcome = MutationOutcome::NoChange;
  result.explanation = Explanation::make("idempotent", "cluster", std::move(detail));
  return result;
}

[[nodiscard]] bool bump(ClusterGeneration& value) noexcept {
  const auto next = value.next();
  if (!next.has_value()) {
    return false;
  }
  value = *next;
  return true;
}

template <class Tag>
[[nodiscard]] bool bump(StrongCounter<Tag>& value) noexcept {
  const auto next = value.next();
  if (!next.has_value()) {
    return false;
  }
  value = *next;
  return true;
}

[[nodiscard]] bool advance_aggregate(DomainGeneration& aggregate,
                                     const DomainGeneration& record) noexcept {
  const std::uint64_t candidate = aggregate.value() + 1;
  const std::uint64_t value = (std::max)(candidate, record.value());
  if (value < aggregate.value() || value == 0) {
    return false;
  }
  aggregate = DomainGeneration::from_raw(value);
  return true;
}

void sort_unique(std::vector<RackId>& racks) {
  std::sort(racks.begin(), racks.end());
  racks.erase(std::unique(racks.begin(), racks.end()), racks.end());
}

void sort_unique_ids(std::vector<FailureDomainId>& ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

/// Normalizes a reference by clearing every purely dynamic field so two
/// publications can be compared on authoritative content alone.
[[nodiscard]] RackReference normalized_for_compare(const RackReference& reference) {
  RackReference value = canonicalize(reference);
  value.publication = PublicationGeneration{};
  value.evidence = EvidenceStamp{};
  value.health = HealthState::Unknown;
  value.currentness = RackCurrentness::Unknown;
  value.coordinator_epoch = CoordinatorEpoch{};
  value.cluster_epoch = ClusterEpoch{};
  for (RackEndpoint& endpoint : value.endpoints) {
    endpoint.evidence = EvidenceStamp{};
  }
  for (RackFailureDomainHint& hint : value.failure_domain_hints) {
    hint.evidence = EvidenceStamp{};
  }
  return value;
}

/// True when the authoritative content is identical. Process incarnation and
/// publisher are compared separately.
[[nodiscard]] bool same_authoritative_content(const RackReference& lhs, const RackReference& rhs) {
  RackReference a = normalized_for_compare(lhs);
  RackReference b = normalized_for_compare(rhs);
  a.boot = b.boot;
  a.publisher = b.publisher;
  return a == b;
}

/// True when this is a byte-identical duplicate from the same incarnation.
[[nodiscard]] bool identical_publication(const RackReference& lhs, const RackReference& rhs) {
  return same_authoritative_content(lhs, rhs) && lhs.boot == rhs.boot &&
         lhs.publisher == rhs.publisher && lhs.publication == rhs.publication &&
         lhs.evidence == rhs.evidence && lhs.health == rhs.health;
}

// -- authority validation ---------------------------------------------------

/// A successful validation gate. MutationResult defaults to REJECTED, so a gate
/// that accepts must say so explicitly: returning a default-constructed result
/// from a validation helper silently rejected every accepted mutation.
[[nodiscard]] MutationResult gate_ok() noexcept {
  MutationResult result;
  result.outcome = MutationOutcome::Accepted;
  return result;
}

[[nodiscard]] MutationResult validate_common(const ApplyContext& ctx,
                                             const MutationRequest& request) {
  if (request.kind == MutationKind::Unknown) {
    return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "request",
                       "mutation kind is UNKNOWN");
  }
  if (!request.cluster.is_valid(request.cluster.view())) {
    return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "cluster",
                       "cluster identity is not valid");
  }
  if (request.kind != MutationKind::DeclareCluster) {
    if (request.cluster != ctx.state.id) {
      return reject_with(RejectionReason::WrongCluster, ErrorStage::ValidateAuthority,
                         "cluster/" + request.cluster.value(),
                         "request targets cluster " + request.cluster.value() + " but this "
                         "coordinator owns " + ctx.state.id.value());
    }
    if (!ctx.state.epoch.known()) {
      return reject_with(RejectionReason::NotReady, ErrorStage::ValidateAuthority, "cluster",
                         "cluster has not been declared");
    }
  }
  if (request.kind == MutationKind::DeclareCluster) {
    if (ctx.state.epoch.known() &&
        request.authority.cluster_epoch != ctx.state.epoch) {
      return reject_with(RejectionReason::StaleClusterEpoch, ErrorStage::ValidateGeneration,
                         "cluster",
                         "declaration carries cluster epoch " +
                             request.authority.cluster_epoch.str() + " but the authoritative "
                             "epoch is " + ctx.state.epoch.str());
    }
    return gate_ok();
  }
  if (request.authority.cluster_epoch != ctx.state.epoch) {
    return reject_with(RejectionReason::StaleClusterEpoch, ErrorStage::ValidateGeneration, "cluster",
                       "request carries cluster epoch " + request.authority.cluster_epoch.str() +
                           " but the authoritative epoch is " + ctx.state.epoch.str());
  }
  if (request.authority.coordinator_epoch != ctx.state.coordinator_epoch) {
    return reject_with(RejectionReason::StaleCoordinatorEpoch, ErrorStage::ValidateGeneration,
                       "cluster",
                       "request carries coordinator epoch " +
                           request.authority.coordinator_epoch.str() +
                           " but the authoritative epoch is " +
                           ctx.state.coordinator_epoch.str());
  }
  if (!accepts_mutations(ctx.state.lifecycle)) {
    return reject_with(RejectionReason::Retired, ErrorStage::ValidateAuthority, "cluster",
                       "cluster is RETIRED and accepts no further mutations");
  }
  return gate_ok();
}

[[nodiscard]] MutationResult validate_boot(const ApplyContext& ctx, const MutationRequest& request,
                                           const RackId& rack) {
  if (!request.authority.boot.has_value()) {
    return gate_ok();
  }
  const RackAgentBootId& boot = *request.authority.boot;
  if (ctx.state.is_boot_fenced(boot)) {
    return reject_with(RejectionReason::StaleRackBoot, ErrorStage::ValidateAuthority,
                       "rack/" + rack.value(),
                       "process incarnation " + boot.value() +
                           " is permanently fenced and cannot mutate cluster state");
  }
  const auto it = ctx.state.racks.find(rack);
  if (it == ctx.state.racks.end()) {
    return gate_ok();
  }
  const RackRecord& record = it->second;
  if (record.authoritative_current && record.reference.boot != boot) {
    return reject_with(RejectionReason::NotAuthorized, ErrorStage::ValidateAuthority,
                       "rack/" + rack.value(),
                       "rack is currently published by process incarnation " +
                           record.reference.boot.value());
  }
  if (request.authority.publisher.has_value() && record.reference.publisher.has_value() &&
      *request.authority.publisher != *record.reference.publisher) {
    return reject_with(RejectionReason::NotAuthorized, ErrorStage::ValidateAuthority,
                       "rack/" + rack.value(),
                       "publisher " + request.authority.publisher->value() +
                           " does not own this rack; owner is " +
                           record.reference.publisher->value());
  }
  if (record.reference.boot == boot && request.authority.publication.known() &&
      request.authority.publication.value() <= record.reference.publication.value()) {
    // An equal publication counter is acceptable only when the request repeats
    // the authoritative content byte for byte: the apply stage answers that
    // duplicate with NO_CHANGE. Anything else at an equal or older counter is a
    // stale replay.
    const bool identical_duplicate =
        request.authority.publication.value() == record.reference.publication.value() &&
        request.rack_reference.rack == rack && request.rack_reference.boot == boot &&
        same_authoritative_content(record.reference, canonicalize(request.rack_reference));
    if (!identical_duplicate) {
      return reject_with(RejectionReason::StalePublication, ErrorStage::ValidateGeneration,
                         "rack/" + rack.value(),
                         "publication " + request.authority.publication.str() +
                             " is not newer than the accepted publication " +
                             record.reference.publication.str(), "stale_publication");
    }
  }
  return gate_ok();
}

// -- rack mutations ---------------------------------------------------------

[[nodiscard]] MutationResult apply_register_publisher(ApplyContext& ctx,
                                                      const MutationRequest& request) {
  const RackId rack = request.authority.rack.value_or(request.rack_reference.rack);
  if (rack.view().empty()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::ValidateReference, "rack",
                       "registration carries no rack identity");
  }
  if (ctx.state.is_rack_retired(rack)) {
    return reject_with(RejectionReason::Retired, ErrorStage::ValidateReference, "rack/" + rack.value(),
                       "rack identity is retired and cannot be re-registered");
  }
  if (!request.authority.publisher.has_value()) {
    return reject_with(RejectionReason::NotAuthorized, ErrorStage::ValidateAuthority,
                       "rack/" + rack.value(), "registration carries no publisher authority");
  }
  const auto existing = ctx.state.racks.find(rack);
  if (existing != ctx.state.racks.end()) {
    if (existing->second.reference.publisher.has_value() &&
        *existing->second.reference.publisher != *request.authority.publisher) {
      return reject_with(RejectionReason::Conflict, ErrorStage::ValidateAuthority,
                         "rack/" + rack.value(),
                         "rack is already owned by publisher " +
                             existing->second.reference.publisher->value());
    }
    if (existing->second.reference.generation.known() &&
        request.authority.rack_generation.has_value() &&
        *request.authority.rack_generation < existing->second.reference.generation) {
      return reject_with(RejectionReason::StaleRackGeneration, ErrorStage::ValidateGeneration,
                         "rack/" + rack.value(),
                         "registration carries generation " +
                             request.authority.rack_generation->str() +
                             " which is older than the authoritative generation " +
                             existing->second.reference.generation.str());
    }
    return accepted_no_change("rack publisher already registered");
  }
  if (ctx.state.racks.size() >= ctx.config.max_racks) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::ValidateReference,
                       "rack/" + rack.value(), "rack count would exceed the configured bound");
  }

  RackRecord record;
  record.reference.rack = rack;
  record.reference.generation =
      request.authority.rack_generation.value_or(request.rack_reference.generation);
  if (!record.reference.generation.known()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::ValidateReference,
                       "rack/" + rack.value(), "registration carries an unknown rack generation");
  }
  record.reference.rack_lifecycle = request.rack_reference.rack_lifecycle;
  record.reference.currentness = RackCurrentness::Unknown;
  record.reference.composition = request.rack_reference.composition;
  record.reference.evidence = request.evidence;
  record.reference.publisher = request.authority.publisher;
  record.reference.publication = request.authority.publication;
  record.reference.boot = request.authority.boot.value_or(request.rack_reference.boot);
  record.reference.cluster_epoch = ctx.state.epoch;
  record.reference.coordinator_epoch = ctx.state.coordinator_epoch;
  record.reference.origin_label = request.rack_reference.origin_label;
  record.membership = RackMembershipState::Unknown;
  record.membership_generation = ctx.state.membership_generation;
  record.accepted_at_generation = ctx.state.generation;
  record.membership_evidence = request.evidence;
  record.authoritative_current = false;
  record.non_authoritative_reason = "registered_but_not_admitted";

  ctx.journal.record_rack(rack);
  ctx.state.racks.insert_or_assign(rack, std::move(record));
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_add_rack(ApplyContext& ctx, const MutationRequest& request) {
  RackReference incoming = canonicalize(request.rack_reference);
  const RackReferenceValidation validation = validate_rack_reference(incoming);
  if (!validation.ok()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::ValidateReference,
                       validation.subject, validation.detail);
  }
  const RackId& rack = incoming.rack;
  if (ctx.state.is_rack_retired(rack)) {
    return reject_with(RejectionReason::Retired, ErrorStage::ValidateReference, "rack/" + rack.value(),
                       "rack identity is retired and cannot be re-admitted");
  }
  MutationResult boot_result = validate_boot(ctx, request, rack);
  if (!boot_result.accepted()) {
    return boot_result;
  }

  const auto existing = ctx.state.racks.find(rack);
  if (existing != ctx.state.racks.end()) {
    const RackRecord& record = existing->second;
    if (incoming.generation < record.reference.generation) {
      return reject_with(RejectionReason::StaleRackGeneration, ErrorStage::ValidateGeneration,
                         "rack/" + rack.value(),
                         "publication carries generation " + incoming.generation.str() +
                             " which is older than the authoritative generation " +
                             record.reference.generation.str());
    }
    // A registered rack that has not been admitted yet holds a placeholder
    // record: it carries no authoritative content, so the first publication at
    // the same generation IS the admission and cannot conflict with anything.
    const bool placeholder =
        record.membership == RackMembershipState::Unknown && !record.authoritative_current;
    if (incoming.generation == record.reference.generation && placeholder) {
      if (record.reference.publisher.has_value() && incoming.publisher.has_value() &&
          *record.reference.publisher != *incoming.publisher) {
        return reject_with(RejectionReason::Conflict, ErrorStage::ValidateAuthority,
                           "rack/" + rack.value(),
                           "registration is owned by publisher " +
                               record.reference.publisher->value());
      }
    } else if (incoming.generation == record.reference.generation) {
      if (!same_authoritative_content(record.reference, incoming)) {
        return reject_with(RejectionReason::Conflict, ErrorStage::ValidateReference,
                           "rack/" + rack.value(),
                           "publication conflicts with the authoritative record at generation " +
                               incoming.generation.str(), "conflicting_rack_publication");
      }
      // A byte-identical republication under the same generation, boot,
      // publisher and publication counter changes nothing authoritative and is
      // answered with NO_CHANGE. Only a duplicate that also advances the
      // publication counter may refresh dynamic observation.
      if (identical_publication(record.reference, incoming) ||
          (same_authoritative_content(record.reference, incoming) &&
           record.reference.boot == incoming.boot &&
           record.reference.publisher == incoming.publisher &&
           record.reference.publication == incoming.publication)) {
        return accepted_no_change("identical rack publication is idempotent");
      }
      if (record.reference.boot == incoming.boot &&
          incoming.publication.value() <= record.reference.publication.value()) {
        return reject_with(RejectionReason::StalePublication, ErrorStage::ValidateGeneration,
                           "rack/" + rack.value(),
                           "dynamic publication is not newer than the accepted publication",
                           "stale_publication");
      }
    } else if (record.reference.publisher.has_value() && incoming.publisher.has_value() &&
               *record.reference.publisher != *incoming.publisher) {
      return reject_with(RejectionReason::NotAuthorized, ErrorStage::ValidateAuthority,
                         "rack/" + rack.value(),
                         "only the owning publisher may advance the rack generation");
    }
  } else if (ctx.state.racks.size() >= ctx.config.max_racks) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::ValidateReference,
                       "rack/" + rack.value(), "rack count would exceed the configured bound");
  }

  RackRecord record = existing == ctx.state.racks.end() ? RackRecord{} : existing->second;
  const bool new_record = existing == ctx.state.racks.end();
  const bool generation_advanced = !new_record && incoming.generation > record.reference.generation;
  const bool membership_changed =
      new_record || record.membership != RackMembershipState::Active;

  record.reference = incoming;
  record.reference.cluster_epoch = ctx.state.epoch;
  record.reference.coordinator_epoch = ctx.state.coordinator_epoch;
  record.reference.currentness = RackCurrentness::Current;
  if (record.reference.evidence.freshness == Freshness::Unknown &&
      record.reference.evidence.observed_at.known()) {
    record.reference.evidence.refresh(ctx.now);
  }
  record.membership = RackMembershipState::Active;
  record.accepted_at_generation = ctx.state.generation;
  record.membership_evidence = record.reference.evidence;
  record.authoritative_current = record.reference.evidence.is_current();
  record.non_authoritative_reason =
      record.authoritative_current ? std::string() : std::string("evidence_not_current");
  if (membership_changed || generation_advanced) {
    if (!bump(ctx.state.membership_generation)) {
      return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, "cluster",
                         "membership generation overflow");
    }
    record.membership_generation = ctx.state.membership_generation;
  }
  if (std::find(ctx.state.withdrawn_racks.begin(), ctx.state.withdrawn_racks.end(), rack) !=
      ctx.state.withdrawn_racks.end()) {
    ctx.state.withdrawn_racks.erase(
        std::remove(ctx.state.withdrawn_racks.begin(), ctx.state.withdrawn_racks.end(), rack),
        ctx.state.withdrawn_racks.end());
  }

  ctx.journal.record_rack(rack);
  ctx.state.racks.insert_or_assign(rack, std::move(record));
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_update_rack_generation(ApplyContext& ctx,
                                                          const MutationRequest& request) {
  const auto rack_id = request.authority.rack.value_or(request.rack_reference.rack);
  if (rack_id.view().empty()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::ValidateReference, "rack",
                       "generation update carries no rack identity");
  }
  const auto it = ctx.state.racks.find(rack_id);
  if (it == ctx.state.racks.end()) {
    return reject_with(RejectionReason::UnknownRack, ErrorStage::ValidateReference,
                       "rack/" + rack_id.value(), "rack is not a member of this cluster");
  }
  MutationResult boot_result = validate_boot(ctx, request, rack_id);
  if (!boot_result.accepted()) {
    return boot_result;
  }
  const RackGeneration current = it->second.reference.generation;
  if (request.expected_rack_generation.has_value() &&
      *request.expected_rack_generation != current) {
    return reject_with(RejectionReason::Conflict, ErrorStage::ValidateGeneration,
                       "rack/" + rack_id.value(),
                       "expected generation " + request.expected_rack_generation->str() +
                           " but the authoritative generation is " + current.str(),
                       "generation_compare_failed");
  }
  if (!request.new_rack_generation.has_value()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::ValidateReference,
                       "rack/" + rack_id.value(), "no new rack generation was supplied");
  }
  if (*request.new_rack_generation < current) {
    return reject_with(RejectionReason::StaleRackGeneration, ErrorStage::ValidateGeneration,
                       "rack/" + rack_id.value(),
                       "requested generation " + request.new_rack_generation->str() +
                           " is older than the authoritative generation " + current.str());
  }
  if (*request.new_rack_generation == current) {
    return accepted_no_change("rack generation already authoritative");
  }

  RackRecord record = it->second;
  record.reference.generation = *request.new_rack_generation;
  record.reference.rack_lifecycle = request.rack_reference.rack_lifecycle;
  if (!request.rack_reference.composition.empty()) {
    record.reference.composition = request.rack_reference.composition;
  }
  if (request.authority.publisher.has_value()) {
    record.reference.publisher = request.authority.publisher;
  }
  record.reference.publication = request.authority.publication;
  if (request.authority.boot.has_value()) {
    record.reference.boot = *request.authority.boot;
  }
  record.reference.evidence = request.evidence;
  record.reference.cluster_epoch = ctx.state.epoch;
  record.reference.coordinator_epoch = ctx.state.coordinator_epoch;
  record.reference.currentness = RackCurrentness::Current;
  record.accepted_at_generation = ctx.state.generation;
  record.authoritative_current = record.reference.evidence.is_current();
  record.non_authoritative_reason =
      record.authoritative_current ? std::string() : std::string("evidence_not_current");
  if (!bump(ctx.state.membership_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, "cluster",
                       "membership generation overflow");
  }
  record.membership_generation = ctx.state.membership_generation;

  ctx.journal.record_rack(rack_id);
  ctx.state.racks.insert_or_assign(rack_id, std::move(record));
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_membership_change(ApplyContext& ctx,
                                                     const MutationRequest& request,
                                                     RackMembershipState target,
                                                     bool add_retired_identity) {
  const auto rack_id = request.authority.rack.value_or(request.rack_reference.rack);
  if (rack_id.view().empty()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::ValidateReference, "rack",
                       "membership change carries no rack identity");
  }
  const auto it = ctx.state.racks.find(rack_id);
  if (it == ctx.state.racks.end()) {
    return reject_with(RejectionReason::UnknownRack, ErrorStage::ValidateReference,
                       "rack/" + rack_id.value(), "rack is not a member of this cluster");
  }
  MutationResult boot_result = validate_boot(ctx, request, rack_id);
  if (!boot_result.accepted()) {
    return boot_result;
  }
  if (it->second.membership == target) {
    return accepted_no_change("membership is already in the requested state");
  }
  if (it->second.membership == RackMembershipState::Retired &&
      target != RackMembershipState::Retired) {
    return reject_with(RejectionReason::Retired, ErrorStage::ValidateAuthority,
                       "rack/" + rack_id.value(),
                       "retired rack identity cannot return to an active membership state");
  }

  RackRecord record = it->second;
  record.membership = target;
  record.membership_evidence = request.evidence;
  if (target != RackMembershipState::Active) {
    record.authoritative_current = false;
    record.reference.currentness = RackCurrentness::RevalidationRequired;
    record.non_authoritative_reason = std::string("membership_") + std::string(to_string(target));
  }
  if (!bump(ctx.state.membership_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, "cluster",
                       "membership generation overflow");
  }
  record.membership_generation = ctx.state.membership_generation;

  const auto withdrawn =
      std::find(ctx.state.withdrawn_racks.begin(), ctx.state.withdrawn_racks.end(), rack_id);
  if (target == RackMembershipState::Withdrawn) {
    if (withdrawn == ctx.state.withdrawn_racks.end()) {
      ctx.state.withdrawn_racks.push_back(rack_id);
      std::sort(ctx.state.withdrawn_racks.begin(), ctx.state.withdrawn_racks.end());
    }
  } else if (withdrawn != ctx.state.withdrawn_racks.end()) {
    ctx.state.withdrawn_racks.erase(withdrawn);
  }
  if (add_retired_identity) {
    const bool present = std::any_of(
        ctx.state.retired_racks.begin(), ctx.state.retired_racks.end(),
        [&rack_id](const RetiredIdentity& retired) { return retired.rack == rack_id; });
    if (!present) {
      RetiredIdentity retired;
      retired.rack = rack_id;
      retired.last_generation = record.reference.generation;
      retired.retired_at = ctx.now;
      retired.reason = request.reason;
      ctx.state.retired_racks.push_back(std::move(retired));
      std::sort(ctx.state.retired_racks.begin(), ctx.state.retired_racks.end(),
                [](const RetiredIdentity& lhs, const RetiredIdentity& rhs) {
                  return lhs.rack < rhs.rack;
                });
    }
  }

  ctx.journal.record_rack(rack_id);
  ctx.state.racks.insert_or_assign(rack_id, std::move(record));
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_recover_rack(ApplyContext& ctx, const MutationRequest& request) {
  RackReference incoming = canonicalize(request.rack_reference);
  const RackId rack = incoming.rack.view().empty()
                          ? request.authority.rack.value_or(request.rack_reference.rack)
                          : incoming.rack;
  if (rack.view().empty()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::ValidateReference, "rack",
                       "recovery carries no rack identity");
  }
  if (ctx.state.is_rack_retired(rack)) {
    return reject_with(RejectionReason::Retired, ErrorStage::ValidateReference, "rack/" + rack.value(),
                       "retired rack identity cannot be recovered");
  }
  const auto it = ctx.state.racks.find(rack);
  if (it == ctx.state.racks.end()) {
    return reject_with(RejectionReason::UnknownRack, ErrorStage::ValidateReference,
                       "rack/" + rack.value(), "rack is not a member of this cluster");
  }
  if (!request.authority.boot.has_value()) {
    return reject_with(RejectionReason::NotAuthorized, ErrorStage::ValidateAuthority,
                       "rack/" + rack.value(),
                       "recovery requires a fresh process incarnation identity");
  }
  const RackAgentBootId& boot = *request.authority.boot;
  if (ctx.state.is_boot_fenced(boot)) {
    return reject_with(RejectionReason::StaleRackBoot, ErrorStage::ValidateAuthority,
                       "rack/" + rack.value(),
                       "process incarnation " + boot.value() + " is permanently fenced");
  }
  if (it->second.reference.boot == boot && !it->second.authoritative_current) {
    return reject_with(RejectionReason::StaleRackBoot, ErrorStage::ValidateAuthority,
                       "rack/" + rack.value(),
                       "recovery must use a fresh process incarnation, not the fenced one");
  }
  const RackGeneration current = it->second.reference.generation;
  RackGeneration target = current;
  if (request.new_rack_generation.has_value()) {
    if (*request.new_rack_generation < current) {
      return reject_with(RejectionReason::StaleRackGeneration, ErrorStage::ValidateGeneration,
                         "rack/" + rack.value(),
                         "recovery generation is older than the authoritative generation");
    }
    target = *request.new_rack_generation;
  }

  RackRecord record = it->second;
  record.reference.rack = rack;
  record.reference.generation = target;
  if (!request.rack_reference.composition.empty()) {
    record.reference.composition = request.rack_reference.composition;
  }
  if (!request.rack_reference.endpoints.empty()) {
    record.reference.endpoints = request.rack_reference.endpoints;
  }
  if (!request.rack_reference.failure_domain_hints.empty()) {
    record.reference.failure_domain_hints = request.rack_reference.failure_domain_hints;
  }
  record.reference.rack_lifecycle = request.rack_reference.rack_lifecycle;
  if (request.authority.publisher.has_value()) {
    record.reference.publisher = request.authority.publisher;
  }
  record.reference.boot = boot;
  record.reference.publication = request.authority.publication;
  record.reference.evidence = request.evidence;
  record.reference.cluster_epoch = ctx.state.epoch;
  record.reference.coordinator_epoch = ctx.state.coordinator_epoch;
  record.reference.currentness = RackCurrentness::Current;
  record.reference.health = request.rack_reference.health;
  record.membership = RackMembershipState::Active;
  record.membership_evidence = request.evidence;
  record.accepted_at_generation = ctx.state.generation;
  record.authoritative_current = request.evidence.is_current();
  record.non_authoritative_reason =
      record.authoritative_current ? std::string() : std::string("evidence_not_current");
  if (!bump(ctx.state.membership_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, "cluster",
                       "membership generation overflow");
  }
  record.membership_generation = ctx.state.membership_generation;
  ctx.state.withdrawn_racks.erase(
      std::remove(ctx.state.withdrawn_racks.begin(), ctx.state.withdrawn_racks.end(), rack),
      ctx.state.withdrawn_racks.end());

  ctx.journal.record_rack(rack);
  ctx.state.racks.insert_or_assign(rack, std::move(record));
  ctx.changed = true;
  return accepted_change();
}

// -- domain mutations -------------------------------------------------------

template <class Domain>
struct DomainTraits;

template <>
struct DomainTraits<PlacementDomain> {
  using Id = PlacementDomainId;
  static constexpr auto member = &ClusterState::placement_domains;
  static constexpr auto aggregate = &DomainGenerations::placement;
  static constexpr const char* label = "placement_domain";
};

template <>
struct DomainTraits<CapacityDomain> {
  using Id = CapacityDomainId;
  static constexpr auto member = &ClusterState::capacity_domains;
  static constexpr auto aggregate = &DomainGenerations::capacity;
  static constexpr const char* label = "capacity_domain";
};

template <>
struct DomainTraits<FailureDomain> {
  using Id = FailureDomainId;
  static constexpr auto member = &ClusterState::failure_domains;
  static constexpr auto aggregate = &DomainGenerations::failure;
  static constexpr const char* label = "failure_domain";
};

template <>
struct DomainTraits<NetworkDomain> {
  using Id = NetworkDomainId;
  static constexpr auto member = &ClusterState::network_domains;
  static constexpr auto aggregate = &DomainGenerations::network;
  static constexpr const char* label = "network_domain";
};

template <>
struct DomainTraits<StorageDomain> {
  using Id = StorageDomainId;
  static constexpr auto member = &ClusterState::storage_domains;
  static constexpr auto aggregate = &DomainGenerations::storage;
  static constexpr const char* label = "storage_domain";
};

template <>
struct DomainTraits<PowerDomain> {
  using Id = PowerDomainId;
  static constexpr auto member = &ClusterState::power_domains;
  static constexpr auto aggregate = &DomainGenerations::power;
  static constexpr const char* label = "power_domain";
};

template <>
struct DomainTraits<CoolingDomain> {
  using Id = CoolingDomainId;
  static constexpr auto member = &ClusterState::cooling_domains;
  static constexpr auto aggregate = &DomainGenerations::cooling;
  static constexpr const char* label = "cooling_domain";
};

template <>
struct DomainTraits<LinkDomain> {
  using Id = LinkDomainId;
  static constexpr auto member = &ClusterState::link_domains;
  static constexpr auto aggregate = &DomainGenerations::link;
  static constexpr const char* label = "link_domain";
};

/// Class-specific validation. The generic case has nothing extra to check.
template <class Domain>
[[nodiscard]] MutationResult validate_domain_extra(ApplyContext& ctx, Domain& domain) {
  (void)ctx;
  (void)domain;
  return gate_ok();
}

/// Capacity quantities must be bounded, uniquely keyed by (unit, provenance)
/// and must carry a usable unit. Numbers with different semantics are never
/// silently combined.
template <>
[[nodiscard]] MutationResult validate_domain_extra<CapacityDomain>(ApplyContext& ctx,
                                                                   CapacityDomain& domain) {
  (void)ctx;
  if (domain.quantities.size() > kMaxCapacityQuantities) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::ValidateReference,
                       "capacity_domain/" + domain.id.value(),
                       "quantity count exceeds the configured bound");
  }
  std::sort(domain.quantities.begin(), domain.quantities.end(),
            [](const CapacityQuantity& lhs, const CapacityQuantity& rhs) {
              if (lhs.unit != rhs.unit) {
                return lhs.unit < rhs.unit;
              }
              if (lhs.provenance != rhs.provenance) {
                return lhs.provenance < rhs.provenance;
              }
              return lhs.aggregated < rhs.aggregated;
            });
  std::string previous_unit;
  bool first = true;
  for (const CapacityQuantity& quantity : domain.quantities) {
    if (quantity.unit.empty() || !validate_label(quantity.unit).ok()) {
      return reject_with(RejectionReason::InvalidDomain, ErrorStage::ValidateReference,
                         "capacity_domain/" + domain.id.value(),
                         "quantity unit is empty or not a valid bounded label");
    }
    if (quantity.value.has_value() && !std::isfinite(quantity.value.value())) {
      return reject_with(RejectionReason::InvalidDomain, ErrorStage::ValidateReference,
                         "capacity_domain/" + domain.id.value(),
                         "quantity value is not a finite number");
    }
    if (!first && quantity.unit == previous_unit) {
      return reject_with(RejectionReason::Conflict, ErrorStage::ValidateReference,
                         "capacity_domain/" + domain.id.value(),
                         "duplicate quantity for unit " + quantity.unit);
    }
    previous_unit = quantity.unit;
    first = false;
  }
  return gate_ok();
}

/// Validates a domain payload before it may enter canonical state.
template <class Domain>
[[nodiscard]] MutationResult validate_domain_payload(ApplyContext& ctx, Domain& domain) {
  using Traits = DomainTraits<Domain>;
  if (domain.racks.size() > kMaxDomainMembers) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::ValidateReference,
                       Traits::label, "domain member count exceeds the configured bound");
  }
  sort_unique(domain.racks);
  for (const RackId& rack : domain.racks) {
    if (!ctx.state.is_rack_member(rack)) {
      return reject_with(RejectionReason::UnknownRack, ErrorStage::ValidateReference,
                         std::string(Traits::label) + "/" + domain.id.value(),
                         "domain references rack " + rack.value() +
                             " which is not a member of this cluster");
    }
  }
  if (!domain.header.generation.known()) {
    return reject_with(RejectionReason::InvalidDomain, ErrorStage::ValidateGeneration,
                       std::string(Traits::label) + "/" + domain.id.value(),
                       "domain generation is zero (unknown)");
  }
  if constexpr (requires(const Domain& value) { value.klass; }) {
    if (static_cast<int>(domain.klass) == 0) {
      return reject_with(RejectionReason::InvalidDomain, ErrorStage::ValidateReference,
                         std::string(Traits::label) + "/" + domain.id.value(),
                         "domain class is UNKNOWN");
    }
  }
  if constexpr (std::is_same_v<Domain, NetworkDomain>) {
    if (static_cast<int>(domain.connectivity) == 0) {
      return reject_with(RejectionReason::InvalidDomain, ErrorStage::ValidateReference,
                         std::string(Traits::label) + "/" + domain.id.value(),
                         "network domain connectivity class is UNKNOWN");
    }
  }
  return validate_domain_extra(ctx, domain);
}

template <class Domain>
[[nodiscard]] MutationResult publish_domain_record(ApplyContext& ctx, Domain domain) {
  using Traits = DomainTraits<Domain>;
  auto& map = ctx.state.*(Traits::member);
  const std::string subject = std::string(Traits::label) + "/" + domain.id.value();

  if (domain.id.view().empty()) {
    return reject_with(RejectionReason::InvalidDomain, ErrorStage::ValidateReference,
                       Traits::label, "domain identity is empty");
  }
  MutationResult payload_result = validate_domain_payload(ctx, domain);
  if (!payload_result.accepted()) {
    return payload_result;
  }

  const auto it = map.find(domain.id);
  if (it != map.end()) {
    const Domain& existing = it->second;
    if (domain.header.generation.value() < existing.header.generation.value()) {
      return reject_with(RejectionReason::StalePublication, ErrorStage::ValidateGeneration, subject,
                         "domain generation " + domain.header.generation.str() +
                             " is older than the authoritative generation " +
                             existing.header.generation.str(),
                         "stale_domain_generation");
    }
    if (domain.header.generation == existing.header.generation) {
      if (domain == existing) {
        return accepted_no_change("identical domain publication is idempotent");
      }
      return reject_with(RejectionReason::Conflict, ErrorStage::ValidateReference, subject,
                         "publication conflicts with the authoritative record at generation " +
                             domain.header.generation.str(),
                         "conflicting_domain_publication");
    }
  } else if (map.size() >= ctx.config.max_domains_per_class) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::ValidateReference, subject,
                       "domain count would exceed the configured bound");
  }

  domain.header.cluster_epoch = ctx.state.epoch;
  domain.header.coordinator_epoch = ctx.state.coordinator_epoch;
  if (!advance_aggregate(ctx.state.domain_generations.*(Traits::aggregate),
                         domain.header.generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, subject,
                       "domain class generation overflow");
  }
  ctx.journal.record(Traits::member, domain.id);
  map.insert_or_assign(domain.id, std::move(domain));
  if (!bump(ctx.state.topology_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, subject,
                       "topology generation overflow");
  }
  ctx.state.topology_record.generation = ctx.state.topology_generation;
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_publish_constraint(ApplyContext& ctx,
                                                      const MutationRequest& request) {
  if (!request.constraint.has_value()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "constraint",
                       "constraint payload is absent");
  }
  ClusterConstraint constraint = *request.constraint;
  const std::string subject = "constraint/" + constraint.id.value();
  if (constraint.racks.size() > kMaxDomainMembers) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::ValidateReference, subject,
                       "constraint member count exceeds the configured bound");
  }
  sort_unique(constraint.racks);
  for (const RackId& rack : constraint.racks) {
    if (!ctx.state.is_rack_member(rack)) {
      return reject_with(RejectionReason::UnknownRack, ErrorStage::ValidateReference, subject,
                         "constraint references rack " + rack.value() + " which is not a member");
    }
  }
  if (constraint.kind == ConstraintKind::Unknown) {
    return reject_with(RejectionReason::InvalidDomain, ErrorStage::ValidateReference, subject,
                       "constraint kind is UNKNOWN");
  }
  if (!constraint.header.generation.known()) {
    return reject_with(RejectionReason::InvalidDomain, ErrorStage::ValidateGeneration, subject,
                       "constraint generation is zero");
  }
  const auto it = ctx.state.constraints.find(constraint.id);
  if (it != ctx.state.constraints.end()) {
    if (constraint.header.generation.value() < it->second.header.generation.value()) {
      return reject_with(RejectionReason::StalePublication, ErrorStage::ValidateGeneration, subject,
                         "constraint generation is older than the authoritative generation",
                         "stale_constraint_generation");
    }
    if (constraint.header.generation == it->second.header.generation) {
      if (constraint == it->second) {
        return accepted_no_change("identical constraint publication is idempotent");
      }
      return reject_with(RejectionReason::Conflict, ErrorStage::ValidateReference, subject,
                         "constraint publication conflicts at the same generation");
    }
  } else if (ctx.state.constraints.size() >= ctx.config.max_constraints) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::ValidateReference, subject,
                       "constraint count would exceed the configured bound");
  }
  constraint.header.cluster_epoch = ctx.state.epoch;
  constraint.header.coordinator_epoch = ctx.state.coordinator_epoch;
  if (!bump(ctx.state.constraint_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, subject,
                       "constraint generation overflow");
  }
  ctx.journal.record(&ClusterState::constraints, constraint.id);
  ctx.state.constraints.insert_or_assign(constraint.id, std::move(constraint));
  ctx.changed = true;
  return accepted_change();
}

// -- topology mutations -----------------------------------------------------

[[nodiscard]] MutationResult apply_publish_link(ApplyContext& ctx, const MutationRequest& request) {
  if (!request.link.has_value()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "link",
                       "link payload is absent");
  }
  InterRackLink link = *request.link;
  const std::string subject = "link/" + link.id.value();
  if (link.source == link.destination) {
    return reject_with(RejectionReason::InvalidRelationship, ErrorStage::ValidateReference, subject,
                       "link connects a rack to itself");
  }
  if (link.direction == LinkDirection::Unknown) {
    return reject_with(RejectionReason::InvalidRelationship, ErrorStage::ValidateReference, subject,
                       "link direction is UNKNOWN");
  }
  if (!ctx.state.is_rack_member(link.source)) {
    return reject_with(RejectionReason::UnknownRack, ErrorStage::ValidateReference, subject,
                       "source rack " + link.source.value() + " is not a member");
  }
  if (!ctx.state.is_rack_member(link.destination)) {
    return reject_with(RejectionReason::UnknownRack, ErrorStage::ValidateReference, subject,
                       "destination rack " + link.destination.value() + " is not a member");
  }
  if (link.failure_domains.size() > kMaxFailureDomainRefs) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::ValidateReference, subject,
                       "link failure domain count exceeds the configured bound");
  }
  sort_unique_ids(link.failure_domains);
  for (const FailureDomainId& domain : link.failure_domains) {
    if (ctx.state.failure_domains.find(domain) == ctx.state.failure_domains.end()) {
      return reject_with(RejectionReason::UnknownDomain, ErrorStage::ValidateReference, subject,
                         "link references unknown failure domain " + domain.value());
    }
  }
  if (link.network_domain.has_value() &&
      ctx.state.network_domains.find(*link.network_domain) == ctx.state.network_domains.end()) {
    return reject_with(RejectionReason::UnknownDomain, ErrorStage::ValidateReference, subject,
                       "link references unknown network domain " + link.network_domain->value());
  }
  if (link.link_domain.has_value() &&
      ctx.state.link_domains.find(*link.link_domain) == ctx.state.link_domains.end()) {
    return reject_with(RejectionReason::UnknownDomain, ErrorStage::ValidateReference, subject,
                       "link references unknown link domain " + link.link_domain->value());
  }
  if (!link.header.generation.known()) {
    return reject_with(RejectionReason::InvalidRelationship, ErrorStage::ValidateGeneration, subject,
                       "link generation is zero");
  }

  const auto existing = ctx.state.links.find(link.id);
  if (existing != ctx.state.links.end()) {
    const InterRackLink& current = existing->second;
    if (link.header.generation.value() < current.header.generation.value()) {
      return reject_with(RejectionReason::StalePublication, ErrorStage::ValidateGeneration, subject,
                         "link generation is older than the authoritative generation",
                         "stale_link_generation");
    }
    if (link.header.generation == current.header.generation) {
      InterRackLink comparable = link;
      InterRackLink previous = current;
      comparable.header.evidence = EvidenceStamp{};
      previous.header.evidence = EvidenceStamp{};
      comparable.publication = PublicationGeneration{};
      previous.publication = PublicationGeneration{};
      if (comparable == previous) {
        return accepted_no_change("identical link publication is idempotent");
      }
      return reject_with(RejectionReason::Conflict, ErrorStage::ValidateReference, subject,
                         "link publication conflicts with the authoritative record at the same "
                         "generation",
                         "conflicting_link_publication");
    }
  } else if (ctx.state.links.size() >= ctx.config.max_links) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::ValidateReference, subject,
                       "link count would exceed the configured bound");
  }

  if (link.topology_epoch != ctx.state.topology_epoch) {
    return reject_with(RejectionReason::StaleTopologyEpoch, ErrorStage::ValidateGeneration, subject,
                       "link carries topology epoch " + link.topology_epoch.str() +
                           " but the authoritative epoch is " + ctx.state.topology_epoch.str());
  }
  link.header.cluster_epoch = ctx.state.epoch;
  link.header.coordinator_epoch = ctx.state.coordinator_epoch;
  link.topology_generation = ctx.state.topology_generation;

  ctx.journal.record(&ClusterState::links, link.id);
  ctx.state.links.insert_or_assign(link.id, std::move(link));
  if (!bump(ctx.state.topology_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, subject,
                       "topology generation overflow");
  }
  if (!bump(ctx.state.connectivity_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, subject,
                       "connectivity generation overflow");
  }
  ctx.state.topology_record.generation = ctx.state.topology_generation;
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_withdraw_link(ApplyContext& ctx, const MutationRequest& request) {
  if (!request.link_id.has_value()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "link",
                       "link identity is absent");
  }
  const auto it = ctx.state.links.find(*request.link_id);
  if (it == ctx.state.links.end()) {
    return accepted_no_change("link is already absent");
  }
  const std::string subject = "link/" + request.link_id->value();
  ctx.journal.record(&ClusterState::links, *request.link_id);
  ctx.state.links.erase(it);
  if (!bump(ctx.state.topology_generation) || !bump(ctx.state.connectivity_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, subject,
                       "generation overflow");
  }
  ctx.state.topology_record.generation = ctx.state.topology_generation;
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_supersede_topology(ApplyContext& ctx,
                                                      const MutationRequest& request) {
  if (!request.target_topology_epoch.has_value()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "topology",
                       "target topology epoch is absent");
  }
  const TopologyEpoch target = *request.target_topology_epoch;
  if (target <= ctx.state.topology_epoch) {
    return reject_with(RejectionReason::StaleTopologyEpoch, ErrorStage::ValidateGeneration,
                       "topology",
                       "target epoch " + target.str() +
                           " is not newer than the authoritative epoch " +
                           ctx.state.topology_epoch.str());
  }
  for (auto& entry : ctx.state.links) {
    ctx.journal.record(&ClusterState::links, entry.first);
    entry.second.header.evidence.mark_revalidation_required();
    entry.second.reachability = Reachability::Unknown;
    entry.second.health = HealthState::Unknown;
  }
  ctx.state.topology_epoch = target;
  if (!bump(ctx.state.topology_generation) || !bump(ctx.state.connectivity_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, "topology",
                       "generation overflow");
  }
  ctx.state.topology_record.epoch = ctx.state.topology_epoch;
  ctx.state.topology_record.generation = ctx.state.topology_generation;
  ctx.state.topology_record.established_at = ctx.now;
  ctx.state.topology_record.evidence =
      EvidenceStamp::make(EvidenceProvenance::Reported, ctx.now, 0);
  ctx.state.topology_record.reason = request.topology_reason.empty()
                                         ? std::string("topology_superseded")
                                         : request.topology_reason;
  ctx.changed = true;
  return accepted_change();
}

// -- evidence mutations -----------------------------------------------------

[[nodiscard]] MutationResult apply_publish_health(ApplyContext& ctx,
                                                  const MutationRequest& request) {
  const auto rack_id = request.authority.rack.value_or(request.rack_reference.rack);
  if (rack_id.view().empty()) {
    return reject_with(RejectionReason::Malformed, ErrorStage::ValidateReference, "rack",
                       "health publication carries no rack identity");
  }
  const auto it = ctx.state.racks.find(rack_id);
  if (it == ctx.state.racks.end()) {
    return reject_with(RejectionReason::UnknownRack, ErrorStage::ValidateReference,
                       "rack/" + rack_id.value(), "rack is not a member of this cluster");
  }
  if (ctx.state.is_rack_retired(rack_id)) {
    return reject_with(RejectionReason::Retired, ErrorStage::ValidateAuthority,
                       "rack/" + rack_id.value(),
                       "the rack identity is retired and cannot be resurrected",
                       "retired_identity");
  }
  MutationResult boot_result = validate_boot(ctx, request, rack_id);
  if (!boot_result.accepted()) {
    return boot_result;
  }
  RackRecord record = it->second;
  if (record.reference.health == request.health &&
      record.reference.evidence.observed_at == request.evidence.observed_at) {
    return accepted_no_change("health publication is identical");
  }
  record.reference.health = request.health;
  record.reference.evidence = request.evidence;
  record.reference.publication = request.authority.publication;
  record.reference.coordinator_epoch = ctx.state.coordinator_epoch;
  record.authoritative_current = request.evidence.is_current();
  record.non_authoritative_reason =
      record.authoritative_current ? std::string() : std::string("evidence_not_current");
  ctx.journal.record_rack(rack_id);
  ctx.state.racks.insert_or_assign(rack_id, std::move(record));

  if (request.reachability != Reachability::Unknown) {
    for (auto& entry : ctx.state.links) {
      InterRackLink& link = entry.second;
      if (link.source != rack_id && link.destination != rack_id) {
        continue;
      }
      ctx.journal.record(&ClusterState::links, entry.first);
      link.reachability = request.reachability;
      link.header.evidence = request.evidence;
    }
    if (!bump(ctx.state.connectivity_generation)) {
      return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit,
                         "rack/" + rack_id.value(), "connectivity generation overflow");
    }
  }
  if (!bump(ctx.state.health_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit,
                       "rack/" + rack_id.value(), "health generation overflow");
  }
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_withdraw_evidence(ApplyContext& ctx,
                                                     const MutationRequest& request) {
  const auto rack_id = request.authority.rack.value_or(request.rack_reference.rack);
  const std::string selector =
      request.evidence_selector.empty() ? std::string("rack_evidence") : request.evidence_selector;
  if (selector != "rack_evidence" && selector != "rack_health" && selector != "reachability") {
    return reject_with(RejectionReason::Malformed, ErrorStage::ValidateReference, "evidence",
                       "unknown evidence selector " + selector);
  }
  const auto it = ctx.state.racks.find(rack_id);
  if (it == ctx.state.racks.end()) {
    return reject_with(RejectionReason::UnknownRack, ErrorStage::ValidateReference,
                       "rack/" + rack_id.value(), "rack is not a member of this cluster");
  }
  RackRecord record = it->second;
  record.reference.evidence.mark_revalidation_required();
  record.reference.currentness = RackCurrentness::RevalidationRequired;
  record.authoritative_current = false;
  record.non_authoritative_reason = "evidence_withdrawn";
  ctx.journal.record_rack(rack_id);
  ctx.state.racks.insert_or_assign(rack_id, std::move(record));

  if (selector == "reachability" || selector == "rack_evidence") {
    for (auto& entry : ctx.state.links) {
      InterRackLink& link = entry.second;
      if (link.source != rack_id && link.destination != rack_id) {
        continue;
      }
      ctx.journal.record(&ClusterState::links, entry.first);
      link.reachability = Reachability::RevalidationRequired;
      link.header.evidence.mark_revalidation_required();
    }
    if (!bump(ctx.state.connectivity_generation)) {
      return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit,
                         "rack/" + rack_id.value(), "connectivity generation overflow");
    }
  }
  if (!bump(ctx.state.health_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit,
                       "rack/" + rack_id.value(), "health generation overflow");
  }
  ctx.changed = true;
  return accepted_change();
}

// -- cluster-level mutations ------------------------------------------------

[[nodiscard]] MutationResult apply_declare_cluster(ApplyContext& ctx, const MutationRequest& request) {
  if (ctx.state.epoch.known()) {
    if (request.readiness_contract == ctx.state.readiness_contract &&
        request.declared_lifecycle == ctx.state.lifecycle) {
      return accepted_no_change("cluster is already declared with this contract");
    }
    ctx.journal.record_scalars();
    ctx.state.readiness_contract = request.readiness_contract;
    ctx.state.lifecycle = request.declared_lifecycle;
    ctx.changed = true;
    return accepted_change();
  }

  const ClusterEpoch epoch =
      request.authority.cluster_epoch.known() ? request.authority.cluster_epoch
                                              : ClusterEpoch::from_raw(1);
  ctx.journal.record_scalars();
  ctx.state.id = request.cluster;
  ctx.state.epoch = epoch;
  ctx.state.coordinator_epoch = request.authority.coordinator_epoch.known()
                                    ? request.authority.coordinator_epoch
                                    : ctx.state.coordinator_epoch;
  ctx.state.generation = ClusterGeneration::from_raw(1);
  ctx.state.membership_generation = MembershipGeneration::from_raw(1);
  ctx.state.topology_epoch = TopologyEpoch::from_raw(1);
  ctx.state.topology_generation = TopologyGeneration::from_raw(1);
  ctx.state.connectivity_generation = ConnectivityGeneration::from_raw(1);
  ctx.state.health_generation = HealthGeneration::from_raw(1);
  ctx.state.constraint_generation = ConstraintGeneration::from_raw(1);
  ctx.state.domain_generations = DomainGenerations{};
  ctx.state.domain_generations.placement = DomainGeneration::from_raw(1);
  ctx.state.domain_generations.capacity = DomainGeneration::from_raw(1);
  ctx.state.domain_generations.failure = DomainGeneration::from_raw(1);
  ctx.state.domain_generations.network = DomainGeneration::from_raw(1);
  ctx.state.domain_generations.storage = DomainGeneration::from_raw(1);
  ctx.state.domain_generations.power = DomainGeneration::from_raw(1);
  ctx.state.domain_generations.cooling = DomainGeneration::from_raw(1);
  ctx.state.domain_generations.link = DomainGeneration::from_raw(1);
  ctx.state.snapshot_generation = SnapshotGeneration::from_raw(1);
  ctx.state.publication_generation = PublicationGeneration::from_raw(1);
  ctx.state.readiness_contract = request.readiness_contract;
  ctx.state.lifecycle = ClusterLifecycle::Declared;
  ctx.state.declared_at = ctx.now;
  ctx.state.last_mutation_at = ctx.now;
  ctx.state.topology_record.epoch = ctx.state.topology_epoch;
  ctx.state.topology_record.generation = ctx.state.topology_generation;
  ctx.state.topology_record.established_at = ctx.now;
  ctx.state.topology_record.evidence =
      EvidenceStamp::make(EvidenceProvenance::Reported, ctx.now, 0);
  ctx.state.topology_record.reason = "cluster_declared";
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_retire_cluster(ApplyContext& ctx,
                                                  const MutationRequest& request) {
  if (ctx.state.lifecycle == ClusterLifecycle::Retired) {
    return accepted_no_change("cluster is already retired");
  }
  ctx.journal.record_scalars();
  ctx.state.lifecycle = ClusterLifecycle::Retired;
  for (auto& entry : ctx.state.racks) {
    if (!entry.second.authoritative_current) {
      continue;
    }
    ctx.journal.record_rack(entry.first);
    entry.second.authoritative_current = false;
    entry.second.reference.currentness = RackCurrentness::RevalidationRequired;
    entry.second.non_authoritative_reason = "cluster_retired";
  }
  (void)request;
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_publish_snapshot(ApplyContext& ctx,
                                                    const MutationRequest& request) {
  (void)request;
  if (!bump(ctx.state.snapshot_generation) || !bump(ctx.state.publication_generation)) {
    return reject_with(RejectionReason::LimitExceeded, ErrorStage::Commit, "snapshot",
                       "snapshot generation overflow");
  }
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_revalidate(ApplyContext& ctx, const MutationRequest& request) {
  std::vector<RackId> candidates = request.racks;
  if (candidates.empty()) {
    for (const auto& entry : ctx.state.racks) {
      candidates.push_back(entry.first);
    }
  }
  if (candidates.empty()) {
    return accepted_no_change("cluster has no member rack to revalidate");
  }
  std::vector<RackId> still_required;
  for (const RackId& rack : candidates) {
    const auto it = ctx.state.racks.find(rack);
    if (it == ctx.state.racks.end()) {
      return reject_with(RejectionReason::UnknownRack, ErrorStage::ValidateReference,
                         "rack/" + rack.value(), "rack is not a member of this cluster");
    }
    const RackRecord& record = it->second;
    const bool fresh = record.authoritative_current &&
                       record.reference.evidence.is_current() &&
                       record.reference.evidence.provenance != EvidenceProvenance::Reconstructed;
    if (!fresh) {
      still_required.push_back(rack);
    }
  }
  if (!still_required.empty()) {
    MutationResult result = MutationResult::rejected(
        RejectionReason::Conflict, ErrorStage::ValidateReference, "cluster",
        std::to_string(still_required.size()) +
            " rack(s) still lack current evidence and must republish under fresh authority",
        "revalidation_incomplete");
    result.outcome = MutationOutcome::RevalidationRequired;
    return result;
  }
  ctx.changed = true;
  return accepted_change();
}

[[nodiscard]] MutationResult apply_request(ApplyContext& ctx, const MutationRequest& request) {
  MutationResult common = validate_common(ctx, request);
  if (common.reason != RejectionReason::None) {
    return common;
  }

  switch (request.kind) {
    case MutationKind::DeclareCluster:
      return apply_declare_cluster(ctx, request);
    case MutationKind::RegisterRackPublisher:
      return apply_register_publisher(ctx, request);
    case MutationKind::AddRack:
      return apply_add_rack(ctx, request);
    case MutationKind::UpdateRackGeneration:
      return apply_update_rack_generation(ctx, request);
    case MutationKind::MarkRackUnavailable:
      return apply_membership_change(ctx, request, RackMembershipState::Unavailable, false);
    case MutationKind::WithdrawRack:
      return apply_membership_change(ctx, request, RackMembershipState::Withdrawn, false);
    case MutationKind::RetireRack:
      return apply_membership_change(ctx, request, RackMembershipState::Retired, true);
    case MutationKind::RecoverRack:
      return apply_recover_rack(ctx, request);
    case MutationKind::PublishInterRackLink:
      return apply_publish_link(ctx, request);
    case MutationKind::WithdrawInterRackLink:
      return apply_withdraw_link(ctx, request);
    case MutationKind::PublishPlacementDomain:
      if (!request.placement_domain.has_value()) {
        return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "placement_domain",
                           "payload is absent");
      }
      return publish_domain_record(ctx, *request.placement_domain);
    case MutationKind::PublishCapacityDomain:
      if (!request.capacity_domain.has_value()) {
        return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "capacity_domain",
                           "payload is absent");
      }
      return publish_domain_record(ctx, *request.capacity_domain);
    case MutationKind::PublishFailureDomain:
      if (!request.failure_domain.has_value()) {
        return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "failure_domain",
                           "payload is absent");
      }
      return publish_domain_record(ctx, *request.failure_domain);
    case MutationKind::PublishNetworkDomain:
      if (!request.network_domain.has_value()) {
        return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "network_domain",
                           "payload is absent");
      }
      return publish_domain_record(ctx, *request.network_domain);
    case MutationKind::PublishStorageDomain:
      if (!request.storage_domain.has_value()) {
        return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "storage_domain",
                           "payload is absent");
      }
      return publish_domain_record(ctx, *request.storage_domain);
    case MutationKind::PublishPowerDomain:
      if (!request.power_domain.has_value()) {
        return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "power_domain",
                           "payload is absent");
      }
      return publish_domain_record(ctx, *request.power_domain);
    case MutationKind::PublishCoolingDomain:
      if (!request.cooling_domain.has_value()) {
        return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "cooling_domain",
                           "payload is absent");
      }
      return publish_domain_record(ctx, *request.cooling_domain);
    case MutationKind::PublishLinkDomain:
      if (!request.link_domain.has_value()) {
        return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "link_domain",
                           "payload is absent");
      }
      return publish_domain_record(ctx, *request.link_domain);
    case MutationKind::PublishConstraint:
      return apply_publish_constraint(ctx, request);
    case MutationKind::PublishHealthCurrentness:
      return apply_publish_health(ctx, request);
    case MutationKind::WithdrawEvidence:
      return apply_withdraw_evidence(ctx, request);
    case MutationKind::SupersedeTopology:
      return apply_supersede_topology(ctx, request);
    case MutationKind::PublishSnapshot:
      return apply_publish_snapshot(ctx, request);
    case MutationKind::RevalidateRecoveredState:
      return apply_revalidate(ctx, request);
    case MutationKind::RetireCluster:
      return apply_retire_cluster(ctx, request);
    case MutationKind::Unknown:
      break;
  }
  return reject_with(RejectionReason::Malformed, ErrorStage::Decode, "request",
                     "unsupported mutation kind");
}

void fill_generations(const ClusterState& state, MutationResult& result) {
  result.cluster_epoch = state.epoch;
  result.coordinator_epoch = state.coordinator_epoch;
  result.cluster_generation = state.generation;
  result.membership_generation = state.membership_generation;
  result.topology_epoch = state.topology_epoch;
  result.topology_generation = state.topology_generation;
  result.connectivity_generation = state.connectivity_generation;
  result.health_generation = state.health_generation;
  result.constraint_generation = state.constraint_generation;
  result.snapshot_generation = state.snapshot_generation;
  result.publication_generation = state.publication_generation;
  result.lifecycle = state.lifecycle;
}

}  // namespace

bool ClusterCoordinator::Impl::persist_now(MutationResult& result) {
  if (store == nullptr || !config.persist_on_commit) {
    return true;
  }
  const PersistedState persisted = detail::to_persisted_state(canonical);
  const PersistenceStore::SaveOutcome outcome = store->save(persisted);
  if (!outcome.ok) {
    ++persistence_failures;
    result = MutationResult::rejected(RejectionReason::PersistenceFailed, ErrorStage::Persist,
                                      "cluster",
                                      outcome.error.message.empty()
                                          ? std::string("durable state commit failed")
                                          : outcome.error.message,
                                      std::string(to_string(outcome.status)));
    return false;
  }
  ++persistence_saves;
  return true;
}

void ClusterCoordinator::Impl::touch_state() {
  ++state_version;
  indexes_valid = false;
}

MutationResult ClusterCoordinator::Impl::apply_and_commit(const MutationRequest& request) {
  CommitHook* const active_hook = hook.load();
  if (active_hook != nullptr) {
    active_hook->before_candidate(request.kind);
  }

  std::lock_guard<std::mutex> lock(state_mutex);
  StateJournal journal(canonical);
  // Snapshot every scalar the commit path may touch (lifecycle and cluster
  // generation in particular) before applying anything, so that a refusal, an
  // invariant violation, or a persistence failure restores the exact prior
  // state including membership and topology generations.
  journal.record_scalars();
  ApplyContext ctx{canonical, config, now(), journal};
  MutationResult result = apply_request(ctx, request);

  if (result.outcome == MutationOutcome::Rejected) {
    journal.rollback();
    ++mutations_rejected;
    switch (result.reason) {
      case RejectionReason::StaleClusterEpoch:
      case RejectionReason::StaleCoordinatorEpoch:
      case RejectionReason::StaleRackBoot:
      case RejectionReason::StaleRackGeneration:
      case RejectionReason::StaleTopologyEpoch:
      case RejectionReason::StalePublication:
        ++stale_replays_rejected;
        break;
      default:
        break;
    }
    fill_generations(canonical, result);
    return result;
  }

  if (result.outcome == MutationOutcome::RevalidationRequired) {
    journal.rollback();
    ++mutations_rejected;
    fill_generations(canonical, result);
    return result;
  }

  if (!ctx.changed) {
    journal.rollback();
    ++mutations_no_change;
    fill_generations(canonical, result);
    return result;
  }

  const ReadinessEvaluation evaluation = evaluate_readiness(canonical);
  canonical.lifecycle = evaluation.lifecycle;

  const InvariantReport report = check_invariants(canonical, nullptr);
  if (!report.ok()) {
    journal.rollback();
    ++mutations_rejected;
    MutationResult violation = MutationResult::rejected(
        RejectionReason::InvariantViolation, ErrorStage::VerifyInvariants, "cluster",
        "candidate state violated an invariant: " + report.describe(), "invariant_violation");
    fill_generations(canonical, violation);
    return violation;
  }

  if (!bump(canonical.generation)) {
    journal.rollback();
    ++mutations_rejected;
    MutationResult overflow =
        MutationResult::rejected(RejectionReason::LimitExceeded, ErrorStage::Commit, "cluster",
                                 "cluster generation overflow", "generation_overflow");
    fill_generations(canonical, overflow);
    return overflow;
  }
  canonical.last_mutation_at = ctx.now;

  if (active_hook != nullptr) {
    active_hook->before_commit(request.kind);
  }

  if (!persist_now(result)) {
    journal.rollback();
    ++mutations_rejected;
    fill_generations(canonical, result);
    return result;
  }

  touch_state();
  ++commits;
  ++mutations_accepted;
  fill_generations(canonical, result);
  result.outcome = MutationOutcome::Accepted;
  result.explanation = Explanation::make("mutation_accepted", "cluster",
                                         std::string(to_string(request.kind)) + " accepted");
  result.explanation.add(std::string(to_string(request.kind)), result.error.subject,
                         "cluster generation " + canonical.generation.str() +
                             " lifecycle " + std::string(to_string(canonical.lifecycle)));
  result.explanation.sort_factors();
  result.explanation.bound_factors();
  result.error = StructuredError::none();
  if (active_hook != nullptr) {
    // The new state is published (touch_state above) and the result is final:
    // this is the observation point a hook may use to record the outcome.
    active_hook->after_publish(request.kind);
  }
  return result;
}

void ClusterCoordinator::Impl::apply_fence(const RackAgentBootId& boot,
                                           std::string_view reason) {
  std::lock_guard<std::mutex> lock(state_mutex);
  StateJournal journal(canonical);
  bool changed = false;
  for (auto& entry : canonical.racks) {
    RackRecord& record = entry.second;
    if (record.reference.boot != boot) {
      continue;
    }
    journal.record_rack(entry.first);
    record.authoritative_current = false;
    record.reference.currentness = RackCurrentness::RevalidationRequired;
    record.reference.evidence.mark_revalidation_required();
    record.non_authoritative_reason = std::string(reason);
    changed = true;
  }
  for (auto& entry : canonical.links) {
    if (!entry.second.boot.has_value() || *entry.second.boot != boot) {
      continue;
    }
    journal.record(&ClusterState::links, entry.first);
    entry.second.reachability = Reachability::Unknown;
    entry.second.health = HealthState::Unknown;
    entry.second.header.evidence.mark_revalidation_required();
    changed = true;
  }
  const bool already_fenced = canonical.is_boot_fenced(boot);
  if (changed && !already_fenced) {
    journal.record_scalars();
    FencedAuthority fenced;
    fenced.boot = boot;
    fenced.rack = RackId::parse("unknown-rack").value();
    for (const auto& entry : canonical.racks) {
      if (entry.second.reference.boot == boot) {
        fenced.rack = entry.first;
        break;
      }
    }
    fenced.reason = std::string(reason);
    fenced.fenced_at = now();
    canonical.fenced_authorities.push_back(std::move(fenced));
    std::sort(canonical.fenced_authorities.begin(), canonical.fenced_authorities.end(),
              [](const FencedAuthority& lhs, const FencedAuthority& rhs) {
                return lhs.boot < rhs.boot;
              });
  }
  if (!changed) {
    journal.rollback();
    return;
  }
  journal.record_scalars();
  const ReadinessEvaluation evaluation = evaluate_readiness(canonical);
  canonical.lifecycle = evaluation.lifecycle;
  if (!bump(canonical.generation) || !bump(canonical.health_generation)) {
    journal.rollback();
    return;
  }
  canonical.last_mutation_at = now();
  MutationResult ignored;
  if (!persist_now(ignored)) {
    journal.rollback();
    return;
  }
  touch_state();
  ++commits;
  ++fence_events;
}

void ClusterCoordinator::Impl::run_commit() {
  for (;;) {
    detail::QueueItem item;
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      queue_condition.wait(lock, [this]() { return !queue.empty() || stopping; });
      if (queue.empty()) {
        if (stopping) {
          return;
        }
        continue;
      }
      item = std::move(queue.front());
      queue.pop_front();
    }

    if (item.kind == detail::QueueItem::Kind::Stop) {
      return;
    }
    if (item.kind == detail::QueueItem::Kind::Fence) {
      apply_fence(item.fence_boot, item.fence_reason);
      continue;
    }
    MutationResult result = apply_and_commit(item.request);
    if (item.slot != nullptr) {
      std::lock_guard<std::mutex> slot_lock(item.slot->mutex);
      item.slot->result = std::move(result);
      item.slot->done = true;
      item.slot->condition.notify_all();
    }
  }
}

}  // namespace cluster_fabric
