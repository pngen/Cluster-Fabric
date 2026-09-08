// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The readiness contract and the cluster lifecycle, evaluated directly on
// constructed canonical state so every branch of evaluate_readiness is
// exercised deterministically and without a coordinator.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "harness.hpp"

#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

namespace {

[[nodiscard]] ClusterId cluster_id() { return *ClusterId::parse("cluster-1"); }
[[nodiscard]] RackId rack_id(const char* text) { return *RackId::parse(text); }
[[nodiscard]] RackPublisherId publisher_id() { return *RackPublisherId::parse("publisher-1"); }
[[nodiscard]] RackAgentBootId boot_id(const char* text) {
  return *RackAgentBootId::parse(text);
}

constexpr std::int64_t kObservedAtMillis = 1'000'000;

/// A stamp that satisfies every currentness requirement.
[[nodiscard]] EvidenceStamp fresh_stamp() {
  return EvidenceStamp::make(EvidenceProvenance::Measured,
                             Timestamp::from_unix_millis(kObservedAtMillis), 60'000);
}

/// A stamp that was observed but is past its time-to-live.
[[nodiscard]] EvidenceStamp stale_stamp() {
  EvidenceStamp stamp = fresh_stamp();
  stamp.freshness = Freshness::Stale;
  return stamp;
}

/// A stamp whose authority is gone.
[[nodiscard]] EvidenceStamp revalidation_stamp() {
  EvidenceStamp stamp = fresh_stamp();
  stamp.mark_revalidation_required();
  return stamp;
}

/// A stamp with no provenance at all.
[[nodiscard]] EvidenceStamp unknown_stamp() { return EvidenceStamp::unknown(); }

/// A canonical state with every counter initialised as a declared cluster has
/// it, so lifecycle evaluation is the only thing under test.
[[nodiscard]] ClusterState make_state(const ReadinessContract& contract) {
  ClusterState state;
  state.id = cluster_id();
  state.epoch = ClusterEpoch::from_raw(1);
  state.coordinator_epoch = CoordinatorEpoch::from_raw(1);
  state.generation = ClusterGeneration::from_raw(2);
  state.membership_generation = MembershipGeneration::from_raw(2);
  state.topology_epoch = TopologyEpoch::from_raw(1);
  state.topology_generation = TopologyGeneration::from_raw(2);
  state.connectivity_generation = ConnectivityGeneration::from_raw(2);
  state.health_generation = HealthGeneration::from_raw(2);
  state.constraint_generation = ConstraintGeneration::from_raw(2);
  state.domain_generations.placement = DomainGeneration::from_raw(1);
  state.domain_generations.capacity = DomainGeneration::from_raw(1);
  state.domain_generations.failure = DomainGeneration::from_raw(1);
  state.domain_generations.network = DomainGeneration::from_raw(1);
  state.domain_generations.storage = DomainGeneration::from_raw(1);
  state.domain_generations.power = DomainGeneration::from_raw(1);
  state.domain_generations.cooling = DomainGeneration::from_raw(1);
  state.domain_generations.link = DomainGeneration::from_raw(1);
  state.snapshot_generation = SnapshotGeneration::from_raw(2);
  state.publication_generation = PublicationGeneration::from_raw(2);
  state.lifecycle = ClusterLifecycle::Declared;
  state.readiness_contract = contract;
  state.topology_record.epoch = state.topology_epoch;
  state.topology_record.generation = state.topology_generation;
  state.topology_record.established_at = Timestamp::from_unix_millis(kObservedAtMillis);
  state.topology_record.evidence =
      EvidenceStamp::make(EvidenceProvenance::Reported, state.topology_record.established_at, 0);
  state.topology_record.reason = "cluster_declared";
  state.declared_at = Timestamp::from_unix_millis(kObservedAtMillis);
  state.last_mutation_at = state.declared_at;
  return state;
}

/// One rack record. current controls both the stored currentness flag and the
/// authoritative_current flag exactly as the commit path would set them.
[[nodiscard]] RackRecord make_record(const char* rack, RackMembershipState membership,
                                     bool current, const EvidenceStamp& stamp) {
  RackRecord record;
  record.reference.rack = rack_id(rack);
  record.reference.generation = RackGeneration::from_raw(1);
  record.reference.rack_lifecycle = RackLifecycleState::Ready;
  // A rack that is merely not current is SUPERSEDED; only evidence that
  // requires revalidation is reported as REVALIDATION_REQUIRED.
  record.reference.currentness =
      current ? RackCurrentness::Current
              : (stamp.requires_revalidation() ? RackCurrentness::RevalidationRequired
                                               : RackCurrentness::Superseded);
  record.reference.evidence = stamp;
  record.reference.publisher = publisher_id();
  record.reference.publication = PublicationGeneration::from_raw(1);
  record.reference.boot = boot_id("boot-1");
  record.reference.health = HealthState::Healthy;
  record.reference.cluster_epoch = ClusterEpoch::from_raw(1);
  record.reference.coordinator_epoch = CoordinatorEpoch::from_raw(1);
  record.reference.origin_label = "rack-fabric:test";
  record.membership = membership;
  record.membership_generation = MembershipGeneration::from_raw(2);
  record.accepted_at_generation = ClusterGeneration::from_raw(2);
  record.membership_evidence = stamp;
  record.authoritative_current = current;
  if (!current) {
    record.non_authoritative_reason = "not_current";
  }
  return record;
}

void add_rack(ClusterState& state, const char* rack, RackMembershipState membership, bool current,
              const EvidenceStamp& stamp) {
  const RackRecord record = make_record(rack, membership, current, stamp);
  state.racks.insert_or_assign(record.reference.rack, record);
}

[[nodiscard]] InterRackLink make_link(const char* id, const char* source, const char* destination,
                                      const EvidenceStamp& stamp) {
  InterRackLink link;
  link.id = *InterRackLinkId::parse(id);
  link.source = rack_id(source);
  link.destination = rack_id(destination);
  link.direction = LinkDirection::Bidirectional;
  link.connectivity = ConnectivityClass::DirectFabric;
  link.reachability = Reachability::Reachable;
  link.health = HealthState::Healthy;
  link.header.generation = DomainGeneration::from_raw(1);
  link.header.evidence = stamp;
  link.topology_epoch = TopologyEpoch::from_raw(1);
  link.topology_generation = TopologyGeneration::from_raw(1);
  return link;
}

void add_placement_domain(ClusterState& state, const char* id, PlacementDomainClass klass,
                          const EvidenceStamp& stamp) {
  PlacementDomain domain;
  domain.id = *PlacementDomainId::parse(id);
  domain.klass = klass;
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = stamp;
  state.placement_domains.insert_or_assign(domain.id, domain);
}

void add_failure_domain(ClusterState& state, const char* id, FailureDomainClass klass,
                        const EvidenceStamp& stamp) {
  FailureDomain domain;
  domain.id = *FailureDomainId::parse(id);
  domain.klass = klass;
  domain.header.generation = DomainGeneration::from_raw(1);
  domain.header.evidence = stamp;
  state.failure_domains.insert_or_assign(domain.id, domain);
}

/// True when blockers are ordered by (code, subject).
[[nodiscard]] bool blockers_are_sorted(const ReadinessEvaluation& evaluation) {
  for (std::size_t index = 1; index < evaluation.blockers.size(); ++index) {
    const ReadinessBlocker& previous = evaluation.blockers.at(index - 1);
    const ReadinessBlocker& current = evaluation.blockers.at(index);
    if (previous.code > current.code) {
      return false;
    }
    if (previous.code == current.code && previous.subject > current.subject) {
      return false;
    }
  }
  return true;
}

/// True when no two blockers share a (code, subject) pair.
[[nodiscard]] bool blockers_are_unique(const ReadinessEvaluation& evaluation) {
  for (std::size_t lhs = 0; lhs < evaluation.blockers.size(); ++lhs) {
    for (std::size_t rhs = lhs + 1; rhs < evaluation.blockers.size(); ++rhs) {
      if (evaluation.blockers.at(lhs).code == evaluation.blockers.at(rhs).code &&
          evaluation.blockers.at(lhs).subject == evaluation.blockers.at(rhs).subject) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] bool has_blocker(const ReadinessEvaluation& evaluation, const std::string& code,
                               const std::string& subject) {
  for (const ReadinessBlocker& blocker : evaluation.blockers) {
    if (blocker.code == code && blocker.subject == subject) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string describe(const ReadinessEvaluation& evaluation) {
  std::string out = "lifecycle=" + std::string(to_string(evaluation.lifecycle)) +
                    " satisfied=" + (evaluation.satisfied ? "true" : "false") + " blockers=";
  for (const ReadinessBlocker& blocker : evaluation.blockers) {
    out += "[";
    out += blocker.code;
    out += "/";
    out += blocker.subject;
    out += "]";
  }
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Contract factories and equality
// ---------------------------------------------------------------------------

CF_TEST(readiness_contract_permissive_and_strict) {
  const ReadinessContract permissive = ReadinessContract::permissive();
  CF_EXPECT_EQ(permissive.minimum_current_racks, std::size_t{0});
  CF_EXPECT(!permissive.require_all_active_racks_current);
  CF_EXPECT(!permissive.require_connectivity_evidence);
  CF_EXPECT_EQ(permissive.minimum_current_links, std::size_t{0});
  CF_EXPECT(permissive.mandatory_racks.empty());
  CF_EXPECT(permissive.required_placement_domain_classes.empty());
  CF_EXPECT(permissive.required_failure_domain_classes.empty());
  CF_EXPECT(permissive.allow_partial);
  CF_EXPECT(permissive.allow_degraded);
  CF_EXPECT(!permissive.require_no_conflicts);

  const ReadinessContract strict = ReadinessContract::strict();
  CF_EXPECT_EQ(strict.minimum_current_racks, std::size_t{1});
  CF_EXPECT(strict.require_all_active_racks_current);
  CF_EXPECT(strict.require_connectivity_evidence);
  CF_EXPECT_EQ(strict.minimum_current_links, std::size_t{1});
  CF_EXPECT(!strict.allow_partial);
  CF_EXPECT(!strict.allow_degraded);
  CF_EXPECT(strict.require_no_conflicts);

  // Equality is field-wise and both factories are deterministic.
  CF_EXPECT(ReadinessContract::permissive() == ReadinessContract::permissive());
  CF_EXPECT(ReadinessContract::strict() == ReadinessContract::strict());
  CF_EXPECT(!(ReadinessContract::permissive() == ReadinessContract::strict()));

  ReadinessContract tweaked = ReadinessContract::permissive();
  tweaked.minimum_current_racks = 3;
  CF_EXPECT(!(tweaked == ReadinessContract::permissive()));
  ReadinessContract with_mandatory = ReadinessContract::permissive();
  with_mandatory.mandatory_racks = {rack_id("rack-a")};
  CF_EXPECT(!(with_mandatory == ReadinessContract::permissive()));
  ReadinessContract with_classes = ReadinessContract::permissive();
  with_classes.required_placement_domain_classes = {PlacementDomainClass::AvailabilityDomain};
  CF_EXPECT(!(with_classes == ReadinessContract::permissive()));
  ReadinessContract with_failure_classes = ReadinessContract::permissive();
  with_failure_classes.required_failure_domain_classes = {FailureDomainClass::Row};
  CF_EXPECT(!(with_failure_classes == ReadinessContract::permissive()));
}

CF_TEST(lifecycle_transition_rules) {
  // Only RETIRED refuses ordinary mutations.
  CF_EXPECT(accepts_mutations(ClusterLifecycle::Declared));
  CF_EXPECT(accepts_mutations(ClusterLifecycle::Forming));
  CF_EXPECT(accepts_mutations(ClusterLifecycle::Partial));
  CF_EXPECT(accepts_mutations(ClusterLifecycle::Ready));
  CF_EXPECT(accepts_mutations(ClusterLifecycle::Degraded));
  CF_EXPECT(accepts_mutations(ClusterLifecycle::RevalidationRequired));
  CF_EXPECT(accepts_mutations(ClusterLifecycle::Retiring));
  CF_EXPECT(!accepts_mutations(ClusterLifecycle::Retired));

  // Only READY, PARTIAL and DEGRADED are consumable.
  CF_EXPECT(!is_consumable_lifecycle(ClusterLifecycle::Declared));
  CF_EXPECT(!is_consumable_lifecycle(ClusterLifecycle::Forming));
  CF_EXPECT(is_consumable_lifecycle(ClusterLifecycle::Partial));
  CF_EXPECT(is_consumable_lifecycle(ClusterLifecycle::Ready));
  CF_EXPECT(is_consumable_lifecycle(ClusterLifecycle::Degraded));
  CF_EXPECT(!is_consumable_lifecycle(ClusterLifecycle::RevalidationRequired));
  CF_EXPECT(!is_consumable_lifecycle(ClusterLifecycle::Retiring));
  CF_EXPECT(!is_consumable_lifecycle(ClusterLifecycle::Retired));

  // Every lifecycle value round-trips through its stable rendering.
  const ClusterLifecycle values[] = {
      ClusterLifecycle::Declared,     ClusterLifecycle::Forming,
      ClusterLifecycle::Partial,      ClusterLifecycle::Ready,
      ClusterLifecycle::Degraded,     ClusterLifecycle::RevalidationRequired,
      ClusterLifecycle::Retiring,     ClusterLifecycle::Retired};
  for (ClusterLifecycle value : values) {
    CF_EXPECT(cluster_lifecycle_from_string(to_string(value)).has_value());
    CF_EXPECT_EQ(*cluster_lifecycle_from_string(to_string(value)), value);
  }
  CF_EXPECT(!cluster_lifecycle_from_string("NOT_A_LIFECYCLE").has_value());

  // Membership, rack lifecycle and currentness also round-trip.
  const RackMembershipState memberships[] = {
      RackMembershipState::Unknown, RackMembershipState::Active, RackMembershipState::Unavailable,
      RackMembershipState::Withdrawn, RackMembershipState::Retired};
  for (RackMembershipState value : memberships) {
    CF_EXPECT_EQ(*rack_membership_state_from_string(to_string(value)), value);
  }
  const RackLifecycleState rack_lifecycles[] = {
      RackLifecycleState::Unknown,  RackLifecycleState::Declared, RackLifecycleState::Forming,
      RackLifecycleState::Ready,    RackLifecycleState::Degraded, RackLifecycleState::Retiring,
      RackLifecycleState::Retired};
  for (RackLifecycleState value : rack_lifecycles) {
    CF_EXPECT_EQ(*rack_lifecycle_state_from_string(to_string(value)), value);
  }
  const RackCurrentness currentness[] = {
      RackCurrentness::Unknown, RackCurrentness::Current, RackCurrentness::Superseded,
      RackCurrentness::RevalidationRequired};
  for (RackCurrentness value : currentness) {
    CF_EXPECT_EQ(*rack_currentness_from_string(to_string(value)), value);
  }
}

// ---------------------------------------------------------------------------
// Lifecycle outcomes
// ---------------------------------------------------------------------------

CF_TEST(readiness_declared_with_no_racks) {
  const ClusterState state = make_state(ReadinessContract::permissive());
  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  CF_EXPECT_EQ(evaluation.lifecycle, ClusterLifecycle::Declared);
  CF_EXPECT(!evaluation.satisfied);
  CF_EXPECT(has_blocker(evaluation, "no_active_rack", "cluster"));
  CF_EXPECT(blockers_are_sorted(evaluation));
  CF_EXPECT(blockers_are_unique(evaluation));

  // A strict contract adds the minimum-current-rack blocker but the lifecycle
  // is still DECLARED: READY is never granted because a cluster exists.
  ClusterState strict_state = make_state(ReadinessContract::strict());
  const ReadinessEvaluation strict_evaluation = evaluate_readiness(strict_state);
  CF_EXPECT_EQ(strict_evaluation.lifecycle, ClusterLifecycle::Declared);
  CF_EXPECT(!strict_evaluation.satisfied);
  CF_EXPECT(has_blocker(strict_evaluation, "no_active_rack", "cluster"));
  CF_EXPECT(has_blocker(strict_evaluation, "minimum_current_racks_not_met", "cluster"));
  CF_EXPECT(has_blocker(strict_evaluation, "connectivity_evidence_missing", "cluster"));
  CF_EXPECT(blockers_are_sorted(strict_evaluation));
}

CF_TEST(readiness_forming_with_active_racks_but_contract_unsatisfied) {
  ReadinessContract contract = ReadinessContract::permissive();
  contract.minimum_current_racks = 1;
  ClusterState state = make_state(contract);
  // ACTIVE but not current, and not marked as requiring revalidation.
  add_rack(state, "rack-a", RackMembershipState::Active, false, stale_stamp());

  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  CF_EXPECT_EQ(evaluation.lifecycle, ClusterLifecycle::Forming);
  CF_EXPECT(!evaluation.satisfied);
  CF_EXPECT(has_blocker(evaluation, "minimum_current_racks_not_met", "cluster"));
  CF_EXPECT(!has_blocker(evaluation, "no_active_rack", "cluster"));
  CF_EXPECT_EQ(state.current_rack_count(), std::size_t{0});
  CF_EXPECT(blockers_are_sorted(evaluation));

  // Two active racks with a two-rack minimum is still FORMING.
  contract.minimum_current_racks = 2;
  ClusterState two = make_state(contract);
  add_rack(two, "rack-a", RackMembershipState::Active, false, stale_stamp());
  add_rack(two, "rack-b", RackMembershipState::Active, false, stale_stamp());
  CF_EXPECT_EQ(evaluate_readiness(two).lifecycle, ClusterLifecycle::Forming);
}

CF_TEST(readiness_ready_when_contract_satisfied_and_required_racks_current) {
  ReadinessContract contract = ReadinessContract::permissive();
  contract.minimum_current_racks = 1;
  ClusterState state = make_state(contract);
  add_rack(state, "rack-a", RackMembershipState::Active, true, fresh_stamp());

  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  CF_EXPECT_EQ(evaluation.lifecycle, ClusterLifecycle::Ready);
  CF_EXPECT(evaluation.satisfied);
  CF_EXPECT(evaluation.blockers.empty());
  CF_EXPECT_EQ(state.current_rack_count(), std::size_t{1});
  CF_EXPECT(is_consumable_lifecycle(evaluation.lifecycle));

  // The strict contract also needs a current link.
  ClusterState strict_state = make_state(ReadinessContract::strict());
  add_rack(strict_state, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  const ReadinessEvaluation without_link = evaluate_readiness(strict_state);
  CF_EXPECT_EQ(without_link.lifecycle, ClusterLifecycle::Degraded);
  CF_EXPECT(has_blocker(without_link, "connectivity_evidence_missing", "cluster"));
  CF_EXPECT(has_blocker(without_link, "minimum_current_links_not_met", "cluster"));

  add_rack(strict_state, "rack-b", RackMembershipState::Active, true, fresh_stamp());
  strict_state.links.insert_or_assign(
      *InterRackLinkId::parse("link-a-b"),
      make_link("link-a-b", "rack-a", "rack-b", fresh_stamp()));
  const ReadinessEvaluation strict_ready = evaluate_readiness(strict_state);
  CF_EXPECT_EQ(strict_ready.lifecycle, ClusterLifecycle::Ready);
  CF_EXPECT(strict_ready.satisfied);
  CF_EXPECT(strict_ready.blockers.empty());

  // A stale link does not count towards connectivity evidence.
  strict_state.links.begin()->second.header.evidence = stale_stamp();
  const ReadinessEvaluation stale_link = evaluate_readiness(strict_state);
  CF_EXPECT_EQ(stale_link.lifecycle, ClusterLifecycle::Degraded);
  CF_EXPECT(has_blocker(stale_link, "connectivity_evidence_missing", "cluster"));
}

CF_TEST(readiness_partial_when_a_rack_is_unavailable_or_withdrawn) {
  ReadinessContract contract = ReadinessContract::permissive();
  ClusterState state = make_state(contract);
  add_rack(state, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  add_rack(state, "rack-b", RackMembershipState::Unavailable, false, stale_stamp());

  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  CF_EXPECT(evaluation.satisfied);
  CF_EXPECT_EQ(evaluation.lifecycle, ClusterLifecycle::Partial);
  CF_EXPECT(is_consumable_lifecycle(evaluation.lifecycle));

  // A withdrawn rack makes the composition knowingly incomplete too.
  ClusterState withdrawn = make_state(contract);
  add_rack(withdrawn, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  add_rack(withdrawn, "rack-b", RackMembershipState::Withdrawn, false, stale_stamp());
  CF_EXPECT_EQ(evaluate_readiness(withdrawn).lifecycle, ClusterLifecycle::Partial);

  // A rack listed in withdrawn_racks is knowingly incomplete even when every
  // record is ACTIVE.
  ClusterState listed = make_state(contract);
  add_rack(listed, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  listed.withdrawn_racks = {rack_id("rack-b")};
  CF_EXPECT_EQ(evaluate_readiness(listed).lifecycle, ClusterLifecycle::Partial);

  // allow_partial=false reports READY instead of PARTIAL for the same state.
  ReadinessContract strict_partial = contract;
  strict_partial.allow_partial = false;
  ClusterState strict_state = make_state(strict_partial);
  add_rack(strict_state, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  add_rack(strict_state, "rack-b", RackMembershipState::Unavailable, false, stale_stamp());
  const ReadinessEvaluation strict_evaluation = evaluate_readiness(strict_state);
  CF_EXPECT(strict_evaluation.satisfied);
  CF_EXPECT_EQ(strict_evaluation.lifecycle, ClusterLifecycle::Ready);

  // A retired rack is not "knowingly incomplete" either.
  ClusterState retired = make_state(contract);
  add_rack(retired, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  add_rack(retired, "rack-b", RackMembershipState::Retired, false, stale_stamp());
  CF_EXPECT_EQ(evaluate_readiness(retired).lifecycle, ClusterLifecycle::Ready);
}

CF_TEST(readiness_degraded_when_unsatisfied_with_current_racks) {
  ReadinessContract contract = ReadinessContract::permissive();
  contract.minimum_current_racks = 2;
  ClusterState state = make_state(contract);
  add_rack(state, "rack-a", RackMembershipState::Active, true, fresh_stamp());

  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  CF_EXPECT_EQ(evaluation.lifecycle, ClusterLifecycle::Degraded);
  CF_EXPECT(!evaluation.satisfied);
  CF_EXPECT(has_blocker(evaluation, "minimum_current_racks_not_met", "cluster"));
  CF_EXPECT_EQ(state.current_rack_count(), std::size_t{1});
  CF_EXPECT(is_consumable_lifecycle(evaluation.lifecycle));
  CF_EXPECT(blockers_are_sorted(evaluation));
}

CF_TEST(readiness_revalidation_required_for_a_required_element) {
  // A mandatory rack that is ACTIVE but not current makes the need fatal.
  ReadinessContract contract = ReadinessContract::permissive();
  contract.mandatory_racks = {rack_id("rack-a")};
  ClusterState state = make_state(contract);
  add_rack(state, "rack-a", RackMembershipState::Active, false, revalidation_stamp());

  const ReadinessEvaluation mandatory = evaluate_readiness(state);
  CF_EXPECT_EQ(mandatory.lifecycle, ClusterLifecycle::RevalidationRequired);
  CF_EXPECT(!mandatory.satisfied);
  CF_EXPECT(has_blocker(mandatory, "mandatory_rack_not_current", "rack/rack-a"));
  CF_EXPECT(has_blocker(mandatory, "rack_revalidation_required", "rack/rack-a"));

  // An unmet minimum current-rack count with any revalidation need is fatal.
  ReadinessContract minimum = ReadinessContract::permissive();
  minimum.minimum_current_racks = 2;
  ClusterState minimum_state = make_state(minimum);
  add_rack(minimum_state, "rack-a", RackMembershipState::Active, false, revalidation_stamp());
  CF_EXPECT_EQ(evaluate_readiness(minimum_state).lifecycle,
               ClusterLifecycle::RevalidationRequired);

  // allow_degraded=false makes any revalidation need fatal, even when the
  // revalidation is not attached to a required element.
  ReadinessContract degraded = ReadinessContract::permissive();
  ClusterState allowed = make_state(degraded);
  add_rack(allowed, "rack-a", RackMembershipState::Active, false, revalidation_stamp());
  const ReadinessEvaluation allowed_evaluation = evaluate_readiness(allowed);
  CF_EXPECT_EQ(allowed_evaluation.lifecycle, ClusterLifecycle::Forming);
  CF_EXPECT(has_blocker(allowed_evaluation, "rack_revalidation_required", "rack/rack-a"));

  ReadinessContract fatal = ReadinessContract::permissive();
  fatal.allow_degraded = false;
  ClusterState fatal_state = make_state(fatal);
  add_rack(fatal_state, "rack-a", RackMembershipState::Active, false, revalidation_stamp());
  const ReadinessEvaluation fatal_evaluation = evaluate_readiness(fatal_state);
  CF_EXPECT_EQ(fatal_evaluation.lifecycle, ClusterLifecycle::RevalidationRequired);
  CF_EXPECT(!fatal_evaluation.satisfied);
  CF_EXPECT(has_blocker(fatal_evaluation, "rack_revalidation_required", "rack/rack-a"));

  // A recovered (RECONSTRUCTED) stamp always requires revalidation.
  ReadinessContract recovered_contract = ReadinessContract::permissive();
  ClusterState recovered = make_state(recovered_contract);
  EvidenceStamp reconstructed = fresh_stamp();
  reconstructed.provenance = EvidenceProvenance::Reconstructed;
  add_rack(recovered, "rack-a", RackMembershipState::Active, false, reconstructed);
  const ReadinessEvaluation recovered_evaluation = evaluate_readiness(recovered);
  CF_EXPECT(has_blocker(recovered_evaluation, "rack_revalidation_required", "rack/rack-a"));

  // A non-ACTIVE rack never contributes a revalidation blocker.
  ClusterState inactive = make_state(ReadinessContract::permissive());
  add_rack(inactive, "rack-a", RackMembershipState::Unavailable, false, revalidation_stamp());
  add_rack(inactive, "rack-b", RackMembershipState::Active, true, fresh_stamp());
  const ReadinessEvaluation inactive_evaluation = evaluate_readiness(inactive);
  CF_EXPECT(!has_blocker(inactive_evaluation, "rack_revalidation_required", "rack/rack-a"));
  CF_EXPECT_EQ(inactive_evaluation.lifecycle, ClusterLifecycle::Partial);
}

CF_TEST(readiness_retired_and_retiring_short_circuit) {
  // A retired cluster is never satisfied, whatever the rack state is.
  ClusterState retired = make_state(ReadinessContract::permissive());
  add_rack(retired, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  retired.lifecycle = ClusterLifecycle::Retired;
  const ReadinessEvaluation retired_evaluation = evaluate_readiness(retired);
  CF_EXPECT_EQ(retired_evaluation.lifecycle, ClusterLifecycle::Retired);
  CF_EXPECT(!retired_evaluation.satisfied);
  CF_EXPECT_EQ(retired_evaluation.blockers.size(), std::size_t{1});
  CF_EXPECT(has_blocker(retired_evaluation, "cluster_retired", "cluster"));
  CF_EXPECT(!accepts_mutations(retired_evaluation.lifecycle));

  // A retiring cluster short-circuits the same way.
  ClusterState retiring = make_state(ReadinessContract::permissive());
  add_rack(retiring, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  retiring.lifecycle = ClusterLifecycle::Retiring;
  const ReadinessEvaluation retiring_evaluation = evaluate_readiness(retiring);
  CF_EXPECT_EQ(retiring_evaluation.lifecycle, ClusterLifecycle::Retiring);
  CF_EXPECT(!retiring_evaluation.satisfied);
  CF_EXPECT(has_blocker(retiring_evaluation, "cluster_retiring", "cluster"));
  CF_EXPECT(!is_consumable_lifecycle(retiring_evaluation.lifecycle));
}

CF_TEST(readiness_mandatory_racks) {
  ReadinessContract contract = ReadinessContract::permissive();
  contract.mandatory_racks = {rack_id("rack-a"), rack_id("rack-b"), rack_id("rack-c")};
  ClusterState state = make_state(contract);
  add_rack(state, "rack-b", RackMembershipState::Unavailable, false, stale_stamp());
  add_rack(state, "rack-c", RackMembershipState::Active, true, fresh_stamp());

  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  CF_EXPECT(!evaluation.satisfied);
  CF_EXPECT(has_blocker(evaluation, "mandatory_rack_missing", "rack/rack-a"));
  CF_EXPECT(has_blocker(evaluation, "mandatory_rack_not_active", "rack/rack-b"));
  CF_EXPECT(!has_blocker(evaluation, "mandatory_rack_not_current", "rack/rack-b"));
  CF_EXPECT(blockers_are_sorted(evaluation));
  CF_EXPECT(blockers_are_unique(evaluation));

  // A present, ACTIVE and CURRENT mandatory rack satisfies its part.
  ClusterState satisfied = make_state(contract);
  add_rack(satisfied, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  add_rack(satisfied, "rack-b", RackMembershipState::Active, true, fresh_stamp());
  add_rack(satisfied, "rack-c", RackMembershipState::Active, true, fresh_stamp());
  const ReadinessEvaluation satisfied_evaluation = evaluate_readiness(satisfied);
  CF_EXPECT(satisfied_evaluation.satisfied);
  CF_EXPECT_EQ(satisfied_evaluation.lifecycle, ClusterLifecycle::Ready);
  CF_EXPECT(satisfied_evaluation.blockers.empty());

  // A mandatory rack that is ACTIVE but not current reports not_current.
  ClusterState not_current = make_state(contract);
  add_rack(not_current, "rack-a", RackMembershipState::Active, false, stale_stamp());
  add_rack(not_current, "rack-b", RackMembershipState::Active, true, fresh_stamp());
  add_rack(not_current, "rack-c", RackMembershipState::Active, true, fresh_stamp());
  const ReadinessEvaluation not_current_evaluation = evaluate_readiness(not_current);
  CF_EXPECT(has_blocker(not_current_evaluation, "mandatory_rack_not_current", "rack/rack-a"));
}

CF_TEST(readiness_minimum_current_racks_counts_only_current_active_racks) {
  ReadinessContract contract = ReadinessContract::permissive();
  contract.minimum_current_racks = 2;
  ClusterState state = make_state(contract);
  add_rack(state, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  add_rack(state, "rack-b", RackMembershipState::Active, false, stale_stamp());
  add_rack(state, "rack-c", RackMembershipState::Unavailable, false, stale_stamp());
  add_rack(state, "rack-d", RackMembershipState::Active, true, fresh_stamp());

  CF_EXPECT_EQ(state.current_rack_count(), std::size_t{2});
  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  CF_EXPECT(evaluation.satisfied);
  CF_EXPECT_EQ(evaluation.lifecycle, ClusterLifecycle::Partial);
  CF_EXPECT(!has_blocker(evaluation, "minimum_current_racks_not_met", "cluster"));

  // Raising the minimum above the current count blocks READY.
  contract.minimum_current_racks = 3;
  state.readiness_contract = contract;
  const ReadinessEvaluation raised = evaluate_readiness(state);
  CF_EXPECT(!raised.satisfied);
  CF_EXPECT(has_blocker(raised, "minimum_current_racks_not_met", "cluster"));
  CF_EXPECT_EQ(raised.lifecycle, ClusterLifecycle::Degraded);
}

CF_TEST(readiness_unknown_evidence_never_counts_as_current) {
  // A stamp with no provenance can never be current.
  CF_EXPECT(!unknown_stamp().is_current());
  CF_EXPECT(!stale_stamp().is_current());
  CF_EXPECT(!revalidation_stamp().is_current());
  CF_EXPECT(fresh_stamp().is_current());
  CF_EXPECT(!EvidenceStamp::unknown().requires_revalidation());
  CF_EXPECT(revalidation_stamp().requires_revalidation());

  // A rack whose evidence is UNKNOWN is not current, so it cannot satisfy the
  // minimum-current-racks requirement.
  ReadinessContract contract = ReadinessContract::permissive();
  contract.minimum_current_racks = 1;
  ClusterState state = make_state(contract);
  add_rack(state, "rack-a", RackMembershipState::Active, unknown_stamp().is_current(),
           unknown_stamp());

  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  CF_EXPECT(!evaluation.satisfied);
  CF_EXPECT_EQ(evaluation.lifecycle, ClusterLifecycle::Forming);
  CF_EXPECT(has_blocker(evaluation, "minimum_current_racks_not_met", "cluster"));
  CF_EXPECT_EQ(state.current_rack_count(), std::size_t{0});

  // require_all_active_racks_current reports the non-current active rack.
  contract.minimum_current_racks = 0;
  contract.require_all_active_racks_current = true;
  ClusterState strict_state = make_state(contract);
  add_rack(strict_state, "rack-a", RackMembershipState::Active, false, unknown_stamp());
  const ReadinessEvaluation strict_evaluation = evaluate_readiness(strict_state);
  CF_EXPECT(!strict_evaluation.satisfied);
  CF_EXPECT(has_blocker(strict_evaluation, "active_rack_not_current", "rack/rack-a"));
  CF_EXPECT_EQ(strict_evaluation.lifecycle, ClusterLifecycle::Forming);

  // A SYNTHETIC or ESTIMATED stamp is also never current.
  CF_EXPECT(!EvidenceStamp::make(EvidenceProvenance::Synthetic,
                                 Timestamp::from_unix_millis(kObservedAtMillis), 60'000)
                 .is_current());
  CF_EXPECT(!EvidenceStamp::make(EvidenceProvenance::Estimated,
                                 Timestamp::from_unix_millis(kObservedAtMillis), 60'000)
                 .is_current());
  CF_EXPECT(!EvidenceStamp::make(EvidenceProvenance::Reconstructed,
                                 Timestamp::from_unix_millis(kObservedAtMillis), 60'000)
                 .is_current());
}

CF_TEST(readiness_domain_and_connectivity_requirements) {
  ReadinessContract contract = ReadinessContract::permissive();
  contract.require_connectivity_evidence = true;
  contract.minimum_current_links = 2;
  contract.required_placement_domain_classes = {PlacementDomainClass::AvailabilityDomain,
                                                PlacementDomainClass::NetworkTier};
  contract.required_failure_domain_classes = {FailureDomainClass::Row};
  ClusterState state = make_state(contract);
  add_rack(state, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  add_rack(state, "rack-b", RackMembershipState::Active, true, fresh_stamp());
  add_placement_domain(state, "placement-1", PlacementDomainClass::AvailabilityDomain,
                       fresh_stamp());
  add_failure_domain(state, "failure-1", FailureDomainClass::Row, fresh_stamp());
  state.links.insert_or_assign(*InterRackLinkId::parse("link-a-b"),
                               make_link("link-a-b", "rack-a", "rack-b", fresh_stamp()));

  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  CF_EXPECT(!evaluation.satisfied);
  CF_EXPECT(has_blocker(evaluation, "minimum_current_links_not_met", "cluster"));
  CF_EXPECT(
      has_blocker(evaluation, "placement_domain_class_missing", "placement_domain_class/NETWORK_TIER"));
  CF_EXPECT(!has_blocker(evaluation, "connectivity_evidence_missing", "cluster"));
  CF_EXPECT(!has_blocker(evaluation, "failure_domain_class_missing",
                         "failure_domain_class/ROW"));
  CF_EXPECT_EQ(evaluation.lifecycle, ClusterLifecycle::Degraded);
  CF_EXPECT(blockers_are_sorted(evaluation));

  // Satisfying every class and link requirement reaches READY.
  add_placement_domain(state, "placement-2", PlacementDomainClass::NetworkTier, fresh_stamp());
  state.links.insert_or_assign(*InterRackLinkId::parse("link-b-a"),
                               make_link("link-b-a", "rack-b", "rack-a", fresh_stamp()));
  const ReadinessEvaluation satisfied = evaluate_readiness(state);
  CF_EXPECT(satisfied.satisfied);
  CF_EXPECT_EQ(satisfied.lifecycle, ClusterLifecycle::Ready);
  CF_EXPECT(satisfied.blockers.empty());

  // A domain whose evidence requires revalidation is not current, so the
  // required class is reported missing again.
  state.placement_domains.at(*PlacementDomainId::parse("placement-2")).header.evidence =
      revalidation_stamp();
  const ReadinessEvaluation revalidating = evaluate_readiness(state);
  CF_EXPECT(has_blocker(revalidating, "placement_domain_class_missing",
                        "placement_domain_class/NETWORK_TIER"));
}

CF_TEST(readiness_structural_conflicts_block_ready) {
  ReadinessContract contract = ReadinessContract::permissive();
  contract.require_no_conflicts = true;
  ClusterState state = make_state(contract);
  add_rack(state, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  add_rack(state, "rack-b", RackMembershipState::Active, true, fresh_stamp());
  state.links.insert_or_assign(*InterRackLinkId::parse("link-dangling"),
                               make_link("link-dangling", "rack-a", "rack-missing", fresh_stamp()));

  const ReadinessEvaluation dangling = evaluate_readiness(state);
  CF_EXPECT(!dangling.satisfied);
  CF_EXPECT(has_blocker(dangling, "structural_conflict", "link/link-dangling"));
  CF_EXPECT_EQ(dangling.lifecycle, ClusterLifecycle::Degraded);

  // A retired rack that is still authoritative current is a conflict.
  ClusterState retired = make_state(contract);
  add_rack(retired, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  RetiredIdentity identity;
  identity.rack = rack_id("rack-a");
  identity.last_generation = RackGeneration::from_raw(1);
  identity.reason = "hardware_removed";
  retired.retired_racks = {identity};
  const ReadinessEvaluation retired_evaluation = evaluate_readiness(retired);
  CF_EXPECT(has_blocker(retired_evaluation, "structural_conflict", "rack/rack-a"));

  // An authoritative current rack published by a fenced incarnation is a
  // conflict too.
  ClusterState fenced = make_state(contract);
  add_rack(fenced, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  FencedAuthority authority;
  authority.boot = boot_id("boot-1");
  authority.rack = rack_id("rack-a");
  authority.reason = "session_closed";
  authority.fenced_at = Timestamp::from_unix_millis(kObservedAtMillis);
  fenced.fenced_authorities = {authority};
  const ReadinessEvaluation fenced_evaluation = evaluate_readiness(fenced);
  CF_EXPECT(has_blocker(fenced_evaluation, "structural_conflict", "rack/rack-a"));

  // With require_no_conflicts=false the same state is READY.
  ReadinessContract permissive = contract;
  permissive.require_no_conflicts = false;
  ClusterState tolerated = make_state(permissive);
  add_rack(tolerated, "rack-a", RackMembershipState::Active, true, fresh_stamp());
  tolerated.retired_racks = {identity};
  const ReadinessEvaluation tolerated_evaluation = evaluate_readiness(tolerated);
  CF_EXPECT(tolerated_evaluation.satisfied);
  CF_EXPECT_EQ(tolerated_evaluation.lifecycle, ClusterLifecycle::Ready);
}

CF_TEST(readiness_blockers_are_sorted_by_code_and_subject) {
  ReadinessContract contract = ReadinessContract::permissive();
  contract.minimum_current_racks = 5;
  contract.mandatory_racks = {rack_id("rack-b"), rack_id("rack-a")};
  contract.required_placement_domain_classes = {PlacementDomainClass::AvailabilityDomain};
  contract.required_failure_domain_classes = {FailureDomainClass::Row};
  contract.require_connectivity_evidence = true;
  ClusterState state = make_state(contract);
  add_rack(state, "rack-a", RackMembershipState::Unavailable, false, stale_stamp());
  add_rack(state, "rack-b", RackMembershipState::Active, false, stale_stamp());

  const ReadinessEvaluation evaluation = evaluate_readiness(state);
  CF_EXPECT(evaluation.blockers.size() >= std::size_t{5});
  CF_EXPECT(blockers_are_sorted(evaluation));
  CF_EXPECT(blockers_are_unique(evaluation));
  if (!blockers_are_sorted(evaluation)) {
    CF_FAIL("blockers are not sorted by (code, subject): " + describe(evaluation));
  }
  // The codes observed for this state, in order.
  std::string codes;
  for (const ReadinessBlocker& blocker : evaluation.blockers) {
    if (!codes.empty()) {
      codes += ",";
    }
    codes += blocker.code;
  }
  CF_EXPECT_EQ(codes,
               std::string("connectivity_evidence_missing,failure_domain_class_missing,"
                           "mandatory_rack_not_active,mandatory_rack_not_current,"
                           "minimum_current_racks_not_met,placement_domain_class_missing"));
  CF_EXPECT(!evaluation.satisfied);
  CF_EXPECT_EQ(evaluation.lifecycle, ClusterLifecycle::RevalidationRequired);
}

int main() { return cf_test::run("test_lifecycle_readiness"); }
