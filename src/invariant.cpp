// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Invariant checking over canonical state and derived indexes.

#include "cluster_fabric/invariant.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "cluster_fabric/limits.hpp"

namespace cluster_fabric {

std::string_view to_string(InvariantId value) noexcept {
  switch (value) {
    case InvariantId::Unknown: return "UNKNOWN";
    case InvariantId::SingleClusterIdentity: return "SINGLE_CLUSTER_IDENTITY";
    case InvariantId::ClusterEpochKnown: return "CLUSTER_EPOCH_KNOWN";
    case InvariantId::CoordinatorEpochKnown: return "COORDINATOR_EPOCH_KNOWN";
    case InvariantId::ClusterGenerationKnown: return "CLUSTER_GENERATION_KNOWN";
    case InvariantId::GenerationsMonotonic: return "GENERATIONS_MONOTONIC";
    case InvariantId::MembershipGenerationConsistent: return "MEMBERSHIP_GENERATION_CONSISTENT";
    case InvariantId::TopologyEpochKnown: return "TOPOLOGY_EPOCH_KNOWN";
    case InvariantId::TopologyGenerationConsistent: return "TOPOLOGY_GENERATION_CONSISTENT";
    case InvariantId::RackIdentitiesValid: return "RACK_IDENTITIES_VALID";
    case InvariantId::RackGenerationAuthoritative: return "RACK_GENERATION_AUTHORITATIVE";
    case InvariantId::RackGenerationUnique: return "RACK_GENERATION_UNIQUE";
    case InvariantId::RackNotFenced: return "RACK_NOT_FENCED";
    case InvariantId::RackNotRetired: return "RACK_NOT_RETIRED";
    case InvariantId::RackMembershipConsistent: return "RACK_MEMBERSHIP_CONSISTENT";
    case InvariantId::RackReferenceBounded: return "RACK_REFERENCE_BOUNDED";
    case InvariantId::LinkEndpointsResolve: return "LINK_ENDPOINTS_RESOLVE";
    case InvariantId::LinkDirectionConsistent: return "LINK_DIRECTION_CONSISTENT";
    case InvariantId::LinkNoSelfLoop: return "LINK_NO_SELF_LOOP";
    case InvariantId::LinkIdentitiesUnique: return "LINK_IDENTITIES_UNIQUE";
    case InvariantId::DomainMembersResolve: return "DOMAIN_MEMBERS_RESOLVE";
    case InvariantId::DomainClassConsistent: return "DOMAIN_CLASS_CONSISTENT";
    case InvariantId::DomainGenerationsConsistent: return "DOMAIN_GENERATIONS_CONSISTENT";
    case InvariantId::ConstraintReferencesResolve: return "CONSTRAINT_REFERENCES_RESOLVE";
    case InvariantId::IndexMatchesCanonical: return "INDEX_MATCHES_CANONICAL";
    case InvariantId::SnapshotMapMatchesState: return "SNAPSHOT_MAP_MATCHES_STATE";
    case InvariantId::LifecycleConsistent: return "LIFECYCLE_CONSISTENT";
    case InvariantId::UnknownNeverPositive: return "UNKNOWN_NEVER_POSITIVE";
    case InvariantId::RecoveredEvidenceNotCurrent: return "RECOVERED_EVIDENCE_NOT_CURRENT";
    case InvariantId::FencedAuthorityConsistent: return "FENCED_AUTHORITY_CONSISTENT";
    case InvariantId::CountsWithinBounds: return "COUNTS_WITHIN_BOUNDS";
    case InvariantId::AccountingBaseline: return "ACCOUNTING_BASELINE";
  }
  return "UNKNOWN";
}

std::string InvariantReport::describe() const {
  if (violations.empty()) {
    return "invariants=ok";
  }
  std::string out = "invariants=violated count=" + std::to_string(violations.size());
  for (const InvariantViolation& violation : violations) {
    out += "\n";
    out += std::string(to_string(violation.id));
    out += " ";
    out += violation.subject;
    out += ": ";
    out += violation.detail;
  }
  return out;
}

namespace {

void add(InvariantReport& report, InvariantId id, std::string subject, std::string detail) {
  InvariantViolation violation;
  violation.id = id;
  violation.subject = std::move(subject);
  violation.detail = std::move(detail);
  report.violations.push_back(std::move(violation));
}

void finish(InvariantReport& report) {
  std::sort(report.violations.begin(), report.violations.end(),
            [](const InvariantViolation& lhs, const InvariantViolation& rhs) {
              if (lhs.id != rhs.id) {
                return lhs.id < rhs.id;
              }
              if (lhs.subject != rhs.subject) {
                return lhs.subject < rhs.subject;
              }
              return lhs.detail < rhs.detail;
            });
}

[[nodiscard]] bool sorted_unique(const std::vector<RackId>& values) {
  return std::is_sorted(values.begin(), values.end()) &&
         std::adjacent_find(values.begin(), values.end()) == values.end();
}

}  // namespace

InvariantReport check_invariants(const ClusterState& state, const ClusterIndexes* indexes) {
  InvariantReport report;

  if (state.racks.empty() && state.links.empty() && state.placement_domains.empty() &&
      state.capacity_domains.empty() && state.failure_domains.empty() &&
      state.network_domains.empty() && state.storage_domains.empty() &&
      state.power_domains.empty() && state.cooling_domains.empty() &&
      state.link_domains.empty() && state.constraints.empty() &&
      state.retired_racks.empty() && state.withdrawn_racks.empty() &&
      state.fenced_authorities.empty()) {
    // Nothing is recorded yet: an undeclared, empty cluster has no cluster-level
    // state that could violate an invariant, whatever identity the coordinator
    // was configured with.
    return report;
  }

  if (state.id.view().empty()) {
    add(report, InvariantId::SingleClusterIdentity, "cluster", "cluster identity is empty");
  }
  if (!state.epoch.known()) {
    add(report, InvariantId::ClusterEpochKnown, "cluster", "cluster epoch is zero");
  }
  if (!state.coordinator_epoch.known()) {
    add(report, InvariantId::CoordinatorEpochKnown, "cluster", "coordinator epoch is zero");
  }
  if (!state.generation.known()) {
    add(report, InvariantId::ClusterGenerationKnown, "cluster", "cluster generation is zero");
  }
  if (!state.membership_generation.known()) {
    add(report, InvariantId::GenerationsMonotonic, "cluster",
        "membership generation is zero");
  }
  if (!state.topology_epoch.known()) {
    add(report, InvariantId::TopologyEpochKnown, "cluster", "topology epoch is zero");
  }
  if (!state.topology_generation.known()) {
    add(report, InvariantId::TopologyGenerationConsistent, "cluster",
        "topology generation is zero");
  }
  if (state.topology_record.epoch != state.topology_epoch) {
    add(report, InvariantId::TopologyGenerationConsistent, "cluster",
        "topology record epoch " + state.topology_record.epoch.str() + " does not match " +
            state.topology_epoch.str());
  }
  if (state.topology_record.generation != state.topology_generation) {
    add(report, InvariantId::TopologyGenerationConsistent, "cluster",
        "topology record generation " + state.topology_record.generation.str() +
            " does not match " + state.topology_generation.str());
  }
  if (state.membership_generation.value() > state.generation.value()) {
    add(report, InvariantId::MembershipGenerationConsistent, "cluster",
        "membership generation exceeds cluster generation");
  }
  if (state.topology_generation.value() > state.generation.value()) {
    add(report, InvariantId::TopologyGenerationConsistent, "cluster",
        "topology generation exceeds cluster generation");
  }

  if (state.racks.size() > kMaxRacksPerCluster) {
    add(report, InvariantId::CountsWithinBounds, "cluster",
        "rack count exceeds the configured bound");
  }
  if (state.links.size() > kMaxInterRackLinks) {
    add(report, InvariantId::CountsWithinBounds, "cluster",
        "link count exceeds the configured bound");
  }
  if (state.constraints.size() > kMaxConstraints) {
    add(report, InvariantId::CountsWithinBounds, "cluster",
        "constraint count exceeds the configured bound");
  }
  const std::size_t domain_counts[] = {
      state.placement_domains.size(), state.capacity_domains.size(),
      state.failure_domains.size(),   state.network_domains.size(),
      state.storage_domains.size(),   state.power_domains.size(),
      state.cooling_domains.size(),   state.link_domains.size()};
  for (std::size_t count : domain_counts) {
    if (count > kMaxDomainsPerClass) {
      add(report, InvariantId::CountsWithinBounds, "cluster",
          "domain count exceeds the configured bound");
      break;
    }
  }

  for (const auto& entry : state.racks) {
    const RackRecord& record = entry.second;
    const std::string subject = "rack/" + entry.first.value();
    if (record.reference.rack != entry.first) {
      add(report, InvariantId::RackIdentitiesValid, subject,
          "record key does not match the referenced rack identity");
    }
    if (!record.reference.generation.known()) {
      add(report, InvariantId::RackGenerationAuthoritative, subject,
          "rack generation is zero");
    }
    if (record.reference.endpoints.size() > kMaxRackEndpoints) {
      add(report, InvariantId::RackReferenceBounded, subject, "endpoint count exceeds the bound");
    }
    if (record.reference.failure_domain_hints.size() > kMaxFailureDomainRefs) {
      add(report, InvariantId::RackReferenceBounded, subject,
          "failure domain hint count exceeds the bound");
    }
    if (record.membership == RackMembershipState::Unknown) {
      // A registered rack that has not been admitted keeps membership UNKNOWN.
      // That is legitimate only while the record stays inert: it must not be
      // authoritative current and it must not appear in topology. Membership
      // UNKNOWN never silently counts as a positive membership.
      if (record.authoritative_current) {
        add(report, InvariantId::RackMembershipConsistent, subject,
            "registered rack with UNKNOWN membership is marked authoritative current");
      }
      if (record.reference.currentness != RackCurrentness::Unknown &&
          record.reference.currentness != RackCurrentness::RevalidationRequired) {
        // UNKNOWN and REVALIDATION_REQUIRED are both "not decided current", so
        // fencing a registered rack may legitimately set the latter.
        add(report, InvariantId::RackMembershipConsistent, subject,
            "registered rack with UNKNOWN membership claims a decided currentness");
      }
    }
    if (record.membership_generation.value() > state.membership_generation.value()) {
      add(report, InvariantId::MembershipGenerationConsistent, subject,
          "record membership generation exceeds the cluster membership generation");
    }
    if (record.authoritative_current) {
      if (record.reference.generation.is_zero()) {
        add(report, InvariantId::RackGenerationAuthoritative, subject,
            "authoritative rack has an unknown generation");
      }
      if (state.is_boot_fenced(record.reference.boot)) {
        add(report, InvariantId::RackNotFenced, subject,
            "authoritative rack is published under a fenced process incarnation");
      }
      if (state.is_rack_retired(entry.first)) {
        add(report, InvariantId::RackNotRetired, subject,
            "retired rack is still authoritative current");
      }
      if (record.reference.evidence.provenance == EvidenceProvenance::Unknown) {
        add(report, InvariantId::UnknownNeverPositive, subject,
            "authoritative rack carries UNKNOWN provenance");
      }
      if (record.reference.evidence.provenance == EvidenceProvenance::Reconstructed) {
        add(report, InvariantId::RecoveredEvidenceNotCurrent, subject,
            "recovered rack evidence is marked authoritative current");
      }
    }
  }

  for (const auto& entry : state.links) {
    const InterRackLink& link = entry.second;
    const std::string subject = "link/" + entry.first.value();
    if (link.id != entry.first) {
      add(report, InvariantId::LinkIdentitiesUnique, subject,
          "record key does not match the link identity");
    }
    if (!state.is_rack_member(link.source)) {
      add(report, InvariantId::LinkEndpointsResolve, subject,
          "source rack " + link.source.value() + " is not a member of this cluster");
    }
    if (!state.is_rack_member(link.destination)) {
      add(report, InvariantId::LinkEndpointsResolve, subject,
          "destination rack " + link.destination.value() + " is not a member of this cluster");
    }
    if (link.source == link.destination) {
      add(report, InvariantId::LinkNoSelfLoop, subject, "link connects a rack to itself");
    }
    if (link.direction == LinkDirection::Unknown) {
      add(report, InvariantId::LinkDirectionConsistent, subject, "link direction is UNKNOWN");
    }
    if (link.topology_epoch != state.topology_epoch &&
        !link.header.evidence.requires_revalidation()) {
      // A link observed under an older topology epoch may stay in the record
      // set after a supersession, but only as a non-authoritative record: it
      // must be explicitly marked revalidation-required and must not claim
      // current reachability or health.
      add(report, InvariantId::TopologyGenerationConsistent, subject,
          "link topology epoch does not match the cluster topology epoch");
    }
    const bool claims_reachability = link.reachability == Reachability::Reachable ||
                                     link.reachability == Reachability::Unreachable;
    if (link.topology_epoch != state.topology_epoch &&
        (claims_reachability || link.health != HealthState::Unknown)) {
      // UNKNOWN and REVALIDATION_REQUIRED are both "not decided" values, so a
      // superseded-epoch link may carry them; a decided reachability or health
      // would be a positive claim about a topology that no longer applies.
      add(report, InvariantId::TopologyGenerationConsistent, subject,
          "link from a superseded topology epoch still claims reachability or health");
    }
    if (link.topology_generation.value() > state.topology_generation.value()) {
      add(report, InvariantId::TopologyGenerationConsistent, subject,
          "link topology generation exceeds the cluster topology generation");
    }
    for (const FailureDomainId& domain : link.failure_domains) {
      if (state.failure_domains.find(domain) == state.failure_domains.end()) {
        add(report, InvariantId::LinkEndpointsResolve, subject,
            "link references unknown failure domain " + domain.value());
      }
    }
  }

  auto check_domain = [&report, &state](const auto& domains, auto class_of, const char* label) {
    for (const auto& entry : domains) {
      const std::string subject = std::string(label) + "/" + entry.first.value();
      if (class_of(entry.second) == 0) {
        add(report, InvariantId::DomainClassConsistent, subject, "domain class is UNKNOWN");
      }
      for (const RackId& rack : entry.second.racks) {
        if (!state.is_rack_member(rack)) {
          add(report, InvariantId::DomainMembersResolve, subject,
              "member rack " + rack.value() + " is not a member of this cluster");
        }
      }
      if (!sorted_unique(entry.second.racks)) {
        add(report, InvariantId::DomainMembersResolve, subject,
            "member rack list is not sorted and unique");
      }
    }
  };

  check_domain(state.placement_domains,
               [](const PlacementDomain& d) {
                 return static_cast<int>(d.klass);
               },
               "placement_domain");
  check_domain(state.capacity_domains,
               [](const CapacityDomain& d) { return static_cast<int>(d.klass); },
               "capacity_domain");
  check_domain(state.failure_domains,
               [](const FailureDomain& d) { return static_cast<int>(d.klass); },
               "failure_domain");
  check_domain(state.network_domains,
               [](const NetworkDomain& d) { return static_cast<int>(d.connectivity); },
               "network_domain");
  check_domain(state.storage_domains,
               [](const StorageDomain&) { return 1; }, "storage_domain");
  check_domain(state.power_domains, [](const PowerDomain&) { return 1; }, "power_domain");
  check_domain(state.cooling_domains, [](const CoolingDomain&) { return 1; }, "cooling_domain");
  check_domain(state.link_domains, [](const LinkDomain&) { return 1; }, "link_domain");

  auto max_generation = [](const auto& domains) {
    std::uint64_t value = 0;
    for (const auto& entry : domains) {
      value = (std::max)(value, entry.second.header.generation.value());
    }
    return value;
  };
  if (max_generation(state.placement_domains) > state.domain_generations.placement.value()) {
    add(report, InvariantId::DomainGenerationsConsistent, "placement_domain",
        "a domain record generation exceeds the aggregate placement generation");
  }
  if (max_generation(state.capacity_domains) > state.domain_generations.capacity.value()) {
    add(report, InvariantId::DomainGenerationsConsistent, "capacity_domain",
        "a domain record generation exceeds the aggregate capacity generation");
  }
  if (max_generation(state.failure_domains) > state.domain_generations.failure.value()) {
    add(report, InvariantId::DomainGenerationsConsistent, "failure_domain",
        "a domain record generation exceeds the aggregate failure generation");
  }
  if (max_generation(state.network_domains) > state.domain_generations.network.value()) {
    add(report, InvariantId::DomainGenerationsConsistent, "network_domain",
        "a domain record generation exceeds the aggregate network generation");
  }
  if (max_generation(state.storage_domains) > state.domain_generations.storage.value()) {
    add(report, InvariantId::DomainGenerationsConsistent, "storage_domain",
        "a domain record generation exceeds the aggregate storage generation");
  }
  if (max_generation(state.power_domains) > state.domain_generations.power.value()) {
    add(report, InvariantId::DomainGenerationsConsistent, "power_domain",
        "a domain record generation exceeds the aggregate power generation");
  }
  if (max_generation(state.cooling_domains) > state.domain_generations.cooling.value()) {
    add(report, InvariantId::DomainGenerationsConsistent, "cooling_domain",
        "a domain record generation exceeds the aggregate cooling generation");
  }
  if (max_generation(state.link_domains) > state.domain_generations.link.value()) {
    add(report, InvariantId::DomainGenerationsConsistent, "link_domain",
        "a domain record generation exceeds the aggregate link generation");
  }

  for (const auto& entry : state.constraints) {
    for (const RackId& rack : entry.second.racks) {
      if (!state.is_rack_member(rack)) {
        add(report, InvariantId::ConstraintReferencesResolve,
            "constraint/" + entry.first.value(),
            "constraint references rack " + rack.value() + " which is not a member");
      }
    }
  }

  if (!sorted_unique(state.withdrawn_racks)) {
    add(report, InvariantId::RackMembershipConsistent, "cluster",
        "withdrawn rack list is not sorted and unique");
  }
  for (const RackId& rack : state.withdrawn_racks) {
    const auto it = state.racks.find(rack);
    if (it == state.racks.end() || it->second.membership != RackMembershipState::Withdrawn) {
      add(report, InvariantId::RackMembershipConsistent, "rack/" + rack.value(),
          "rack is listed as withdrawn but its membership state disagrees");
    }
  }
  for (const auto& entry : state.racks) {
    if (entry.second.membership == RackMembershipState::Withdrawn &&
        !std::binary_search(state.withdrawn_racks.begin(), state.withdrawn_racks.end(),
                            entry.first)) {
      add(report, InvariantId::RackMembershipConsistent, "rack/" + entry.first.value(),
          "membership is withdrawn but the rack is absent from the withdrawn list");
    }
  }

  for (std::size_t i = 1; i < state.fenced_authorities.size(); ++i) {
    if (!(state.fenced_authorities[i - 1].boot < state.fenced_authorities[i].boot)) {
      add(report, InvariantId::FencedAuthorityConsistent, "cluster",
          "fenced authority list is not strictly sorted by boot identity");
      break;
    }
  }
  for (const FencedAuthority& fenced : state.fenced_authorities) {
    const auto it = state.racks.find(fenced.rack);
    if (it != state.racks.end() && it->second.authoritative_current &&
        it->second.reference.boot == fenced.boot) {
      add(report, InvariantId::RackNotFenced, "rack/" + fenced.rack.value(),
          "fenced process incarnation is still authoritative current");
    }
  }

  for (std::size_t i = 1; i < state.retired_racks.size(); ++i) {
    if (!(state.retired_racks[i - 1].rack < state.retired_racks[i].rack)) {
      add(report, InvariantId::RackNotRetired, "cluster",
          "retired identity list is not strictly sorted by rack identity");
      break;
    }
  }

  if (state.lifecycle == ClusterLifecycle::Ready) {
    const ReadinessEvaluation evaluation = evaluate_readiness(state);
    if (!evaluation.satisfied) {
      add(report, InvariantId::LifecycleConsistent, "cluster",
          "cluster is READY but the readiness contract is not satisfied");
    }
  }
  if (state.lifecycle == ClusterLifecycle::Retired && !state.racks.empty()) {
    // A retired cluster keeps its history but must not be authoritative
    // current for any rack.
    for (const auto& entry : state.racks) {
      if (entry.second.authoritative_current) {
        add(report, InvariantId::LifecycleConsistent, "rack/" + entry.first.value(),
            "rack is authoritative current in a RETIRED cluster");
      }
    }
  }

  if (state.snapshot_generation.value() > state.publication_generation.value()) {
    add(report, InvariantId::SnapshotMapMatchesState, "cluster",
        "snapshot generation exceeds the publication generation");
  }

  if (indexes != nullptr) {
    const InvariantReport index_report = check_indexes(state, *indexes);
    for (const InvariantViolation& violation : index_report.violations) {
      report.violations.push_back(violation);
    }
  }

  finish(report);
  return report;
}

InvariantReport check_indexes(const ClusterState& state, const ClusterIndexes& indexes) {
  InvariantReport report;

  for (const auto& entry : state.placement_domains) {
    std::vector<RackId> expected = entry.second.racks;
    std::sort(expected.begin(), expected.end());
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
    if (indexes.racks_in_placement_domain(entry.first) != expected) {
      add(report, InvariantId::IndexMatchesCanonical,
          "placement_domain/" + entry.first.value(),
          "index member list does not match the canonical record");
    }
  }
  for (const auto& entry : state.capacity_domains) {
    std::vector<RackId> expected = entry.second.racks;
    std::sort(expected.begin(), expected.end());
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
    if (indexes.racks_in_capacity_domain(entry.first) != expected) {
      add(report, InvariantId::IndexMatchesCanonical,
          "capacity_domain/" + entry.first.value(),
          "index member list does not match the canonical record");
    }
  }
  for (const auto& entry : state.failure_domains) {
    std::vector<RackId> expected = entry.second.racks;
    std::sort(expected.begin(), expected.end());
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
    if (indexes.racks_in_failure_domain(entry.first) != expected) {
      add(report, InvariantId::IndexMatchesCanonical,
          "failure_domain/" + entry.first.value(),
          "index member list does not match the canonical record");
    }
  }
  for (const auto& entry : state.network_domains) {
    std::vector<RackId> expected = entry.second.racks;
    std::sort(expected.begin(), expected.end());
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
    if (indexes.racks_in_network_domain(entry.first) != expected) {
      add(report, InvariantId::IndexMatchesCanonical, "network_domain/" + entry.first.value(),
          "index member list does not match the canonical record");
    }
  }

  for (const auto& entry : state.racks) {
    std::vector<PlacementDomainId> expected_placement;
    for (const auto& domain : state.placement_domains) {
      if (std::binary_search(domain.second.racks.begin(), domain.second.racks.end(), entry.first)) {
        expected_placement.push_back(domain.first);
      }
    }
    if (indexes.placement_domains_of(entry.first) != expected_placement) {
      add(report, InvariantId::IndexMatchesCanonical, "rack/" + entry.first.value(),
          "placement domain index does not match canonical records");
    }
    std::vector<FailureDomainId> expected_failure;
    for (const auto& domain : state.failure_domains) {
      if (std::binary_search(domain.second.racks.begin(), domain.second.racks.end(), entry.first)) {
        expected_failure.push_back(domain.first);
      }
    }
    if (indexes.failure_domains_of(entry.first) != expected_failure) {
      add(report, InvariantId::IndexMatchesCanonical, "rack/" + entry.first.value(),
          "failure domain index does not match canonical records");
    }
  }

  for (const auto& entry : state.links) {
    const std::vector<InterRackLinkId>& from = indexes.links_from(entry.second.source);
    if (!std::binary_search(from.begin(), from.end(), entry.first)) {
      add(report, InvariantId::IndexMatchesCanonical, "link/" + entry.first.value(),
          "source index is missing this link");
    }
    const std::vector<InterRackLinkId>& to = indexes.links_to(entry.second.destination);
    if (!std::binary_search(to.begin(), to.end(), entry.first)) {
      add(report, InvariantId::IndexMatchesCanonical, "link/" + entry.first.value(),
          "destination index is missing this link");
    }
  }

  std::map<RackMembershipState, std::vector<RackId>> expected_membership;
  std::map<RackAgentBootId, std::vector<RackId>> expected_boot;
  for (const auto& entry : state.racks) {
    expected_membership[entry.second.membership].push_back(entry.first);
    expected_boot[entry.second.reference.boot].push_back(entry.first);
  }
  for (const auto& entry : expected_membership) {
    if (indexes.racks_in_membership_state(entry.first) != entry.second) {
      add(report, InvariantId::IndexMatchesCanonical, "cluster",
          "membership index does not match canonical records");
      break;
    }
  }
  for (const auto& entry : expected_boot) {
    if (indexes.racks_for_boot(entry.first) != entry.second) {
      add(report, InvariantId::IndexMatchesCanonical, "boot/" + entry.first.value(),
          "boot index does not match canonical records");
      break;
    }
  }

  finish(report);
  return report;
}

}  // namespace cluster_fabric
