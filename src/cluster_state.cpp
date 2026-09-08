// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Canonical cluster state, derived indexes, deterministic queries and the
// readiness contract evaluation.

#include "cluster_fabric/cluster_state.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "cluster_fabric/limits.hpp"

namespace cluster_fabric {

namespace {

template <class T>
[[nodiscard]] const std::vector<T>& empty_vector() noexcept {
  static const std::vector<T> kEmpty{};
  return kEmpty;
}

[[nodiscard]] bool is_active(const RackRecord& record) noexcept {
  return record.membership == RackMembershipState::Active;
}

[[nodiscard]] bool is_current_active(const RackRecord& record) noexcept {
  return is_active(record) && record.authoritative_current;
}

[[nodiscard]] bool needs_revalidation(const RackRecord& record) noexcept {
  if (!is_active(record)) {
    return false;
  }
  if (record.reference.currentness == RackCurrentness::RevalidationRequired) {
    return true;
  }
  return record.reference.evidence.requires_revalidation();
}

void add_blocker(ReadinessEvaluation& evaluation, std::string code, std::string subject,
                 std::string detail) {
  ReadinessBlocker blocker;
  blocker.code = std::move(code);
  blocker.subject = std::move(subject);
  blocker.detail = std::move(detail);
  evaluation.blockers.push_back(std::move(blocker));
}

[[nodiscard]] bool domain_is_current(const DomainHeader& header) noexcept {
  return header.evidence.is_current();
}

[[nodiscard]] bool domain_needs_revalidation(const DomainHeader& header) noexcept {
  return header.evidence.requires_revalidation();
}

}  // namespace

bool ClusterState::is_boot_fenced(const RackAgentBootId& boot) const noexcept {
  for (const FencedAuthority& fenced : fenced_authorities) {
    if (fenced.boot == boot) {
      return true;
    }
  }
  return false;
}

bool ClusterState::is_rack_retired(const RackId& rack) const noexcept {
  for (const RetiredIdentity& retired : retired_racks) {
    if (retired.rack == rack) {
      return true;
    }
  }
  return false;
}

std::size_t ClusterState::current_rack_count() const noexcept {
  std::size_t count = 0;
  for (const auto& entry : racks) {
    if (is_current_active(entry.second)) {
      ++count;
    }
  }
  return count;
}

std::string_view to_string(CapacityAggregateStatus value) noexcept {
  switch (value) {
    case CapacityAggregateStatus::Unknown: return "UNKNOWN";
    case CapacityAggregateStatus::Known: return "KNOWN";
    case CapacityAggregateStatus::MixedSemantics: return "MIXED_SEMANTICS";
    case CapacityAggregateStatus::IncompleteEvidence: return "INCOMPLETE_EVIDENCE";
    case CapacityAggregateStatus::RevalidationRequired: return "REVALIDATION_REQUIRED";
  }
  return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// Derived indexes
// ---------------------------------------------------------------------------

void ClusterIndexes::rebuild(const ClusterState& state) {
  racks_in_placement_.clear();
  racks_in_capacity_.clear();
  racks_in_failure_.clear();
  racks_in_network_.clear();
  placement_of_rack_.clear();
  capacity_of_rack_.clear();
  failure_of_rack_.clear();
  network_of_rack_.clear();
  links_from_.clear();
  links_to_.clear();
  placement_by_class_.clear();
  failure_by_class_.clear();
  racks_by_membership_.clear();
  racks_by_boot_.clear();
  rack_count_ = state.racks.size();

  for (const auto& entry : state.racks) {
    racks_by_membership_[entry.second.membership].push_back(entry.first);
    racks_by_boot_[entry.second.reference.boot].push_back(entry.first);
  }

  for (const auto& entry : state.placement_domains) {
    std::vector<RackId> members = entry.second.racks;
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    racks_in_placement_[entry.first] = members;
    placement_by_class_[entry.second.klass].push_back(entry.first);
    for (const RackId& rack : members) {
      placement_of_rack_[rack].push_back(entry.first);
    }
  }

  for (const auto& entry : state.capacity_domains) {
    std::vector<RackId> members = entry.second.racks;
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    racks_in_capacity_[entry.first] = members;
    for (const RackId& rack : members) {
      capacity_of_rack_[rack].push_back(entry.first);
    }
  }

  for (const auto& entry : state.failure_domains) {
    std::vector<RackId> members = entry.second.racks;
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    racks_in_failure_[entry.first] = members;
    failure_by_class_[entry.second.klass].push_back(entry.first);
    for (const RackId& rack : members) {
      failure_of_rack_[rack].push_back(entry.first);
    }
  }

  for (const auto& entry : state.network_domains) {
    std::vector<RackId> members = entry.second.racks;
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    racks_in_network_[entry.first] = members;
    for (const RackId& rack : members) {
      network_of_rack_[rack].push_back(entry.first);
    }
  }

  for (const auto& entry : state.links) {
    links_from_[entry.second.source].push_back(entry.first);
    if (entry.second.destination != entry.second.source) {
      links_to_[entry.second.destination].push_back(entry.first);
    } else {
      links_to_[entry.second.destination].push_back(entry.first);
    }
  }

  for (auto& entry : links_from_) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  for (auto& entry : links_to_) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  for (auto& entry : placement_of_rack_) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  for (auto& entry : capacity_of_rack_) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  for (auto& entry : failure_of_rack_) {
    std::sort(entry.second.begin(), entry.second.end());
  }
  for (auto& entry : network_of_rack_) {
    std::sort(entry.second.begin(), entry.second.end());
  }
}

const std::vector<RackId>& ClusterIndexes::racks_in_placement_domain(
    const PlacementDomainId& id) const noexcept {
  const auto it = racks_in_placement_.find(id);
  return it == racks_in_placement_.end() ? empty_vector<RackId>() : it->second;
}

const std::vector<RackId>& ClusterIndexes::racks_in_capacity_domain(
    const CapacityDomainId& id) const noexcept {
  const auto it = racks_in_capacity_.find(id);
  return it == racks_in_capacity_.end() ? empty_vector<RackId>() : it->second;
}

const std::vector<RackId>& ClusterIndexes::racks_in_failure_domain(
    const FailureDomainId& id) const noexcept {
  const auto it = racks_in_failure_.find(id);
  return it == racks_in_failure_.end() ? empty_vector<RackId>() : it->second;
}

const std::vector<RackId>& ClusterIndexes::racks_in_network_domain(
    const NetworkDomainId& id) const noexcept {
  const auto it = racks_in_network_.find(id);
  return it == racks_in_network_.end() ? empty_vector<RackId>() : it->second;
}

const std::vector<PlacementDomainId>& ClusterIndexes::placement_domains_of(
    const RackId& rack) const noexcept {
  const auto it = placement_of_rack_.find(rack);
  return it == placement_of_rack_.end() ? empty_vector<PlacementDomainId>() : it->second;
}

const std::vector<CapacityDomainId>& ClusterIndexes::capacity_domains_of(
    const RackId& rack) const noexcept {
  const auto it = capacity_of_rack_.find(rack);
  return it == capacity_of_rack_.end() ? empty_vector<CapacityDomainId>() : it->second;
}

const std::vector<FailureDomainId>& ClusterIndexes::failure_domains_of(
    const RackId& rack) const noexcept {
  const auto it = failure_of_rack_.find(rack);
  return it == failure_of_rack_.end() ? empty_vector<FailureDomainId>() : it->second;
}

const std::vector<NetworkDomainId>& ClusterIndexes::network_domains_of(
    const RackId& rack) const noexcept {
  const auto it = network_of_rack_.find(rack);
  return it == network_of_rack_.end() ? empty_vector<NetworkDomainId>() : it->second;
}

const std::vector<InterRackLinkId>& ClusterIndexes::links_from(const RackId& rack) const noexcept {
  const auto it = links_from_.find(rack);
  return it == links_from_.end() ? empty_vector<InterRackLinkId>() : it->second;
}

const std::vector<InterRackLinkId>& ClusterIndexes::links_to(const RackId& rack) const noexcept {
  const auto it = links_to_.find(rack);
  return it == links_to_.end() ? empty_vector<InterRackLinkId>() : it->second;
}

const std::vector<PlacementDomainId>& ClusterIndexes::placement_domains_of_class(
    PlacementDomainClass klass) const noexcept {
  const auto it = placement_by_class_.find(klass);
  return it == placement_by_class_.end() ? empty_vector<PlacementDomainId>() : it->second;
}

const std::vector<FailureDomainId>& ClusterIndexes::failure_domains_of_class(
    FailureDomainClass klass) const noexcept {
  const auto it = failure_by_class_.find(klass);
  return it == failure_by_class_.end() ? empty_vector<FailureDomainId>() : it->second;
}

const std::vector<RackId>& ClusterIndexes::racks_in_membership_state(
    RackMembershipState state) const noexcept {
  const auto it = racks_by_membership_.find(state);
  return it == racks_by_membership_.end() ? empty_vector<RackId>() : it->second;
}

const std::vector<RackId>& ClusterIndexes::racks_for_boot(const RackAgentBootId& boot) const {
  const auto it = racks_by_boot_.find(boot);
  return it == racks_by_boot_.end() ? empty_vector<RackId>() : it->second;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

CapacityAggregate aggregate_capacity(const ClusterState& state, const CapacityDomainId& domain,
                                     std::string_view unit) {
  CapacityAggregate result;
  result.unit = std::string(unit);
  const auto it = state.capacity_domains.find(domain);
  if (it == state.capacity_domains.end()) {
    result.status = CapacityAggregateStatus::Unknown;
    result.reason = "unknown_capacity_domain";
    return result;
  }
  const CapacityDomain& record = it->second;

  bool found_any = false;
  bool any_unknown_value = false;
  bool any_revalidation = false;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  bool provenance_set = false;
  bool mixed = false;
  bool aggregated_seen = false;
  bool aggregated_unset = false;
  double total = 0.0;
  std::size_t contributing = 0;

  for (const CapacityQuantity& quantity : record.quantities) {
    if (quantity.unit != unit) {
      continue;
    }
    found_any = true;
    if (quantity.provenance == EvidenceProvenance::Reconstructed) {
      any_revalidation = true;
    }
    if (!provenance_set) {
      provenance = quantity.provenance;
      provenance_set = true;
    } else if (provenance != quantity.provenance) {
      mixed = true;
    }
    if (quantity.aggregated) {
      aggregated_seen = true;
    } else {
      aggregated_unset = true;
    }
    if (!quantity.value.has_value()) {
      any_unknown_value = true;
      continue;
    }
    total += *quantity.value;
    ++contributing;
  }

  if (!found_any) {
    result.status = CapacityAggregateStatus::Unknown;
    result.reason = "no_quantity_with_unit";
    return result;
  }
  if (any_revalidation) {
    result.status = CapacityAggregateStatus::RevalidationRequired;
    result.reason = "reconstructed_quantity_requires_revalidation";
    return result;
  }
  if (mixed || (aggregated_seen && aggregated_unset)) {
    result.status = CapacityAggregateStatus::MixedSemantics;
    result.reason = "member_quantities_have_different_semantics";
    return result;
  }

  // Member racks must exist in canonical state before an aggregate may be
  // presented as meaningful.
  for (const RackId& rack : record.racks) {
    const auto rack_it = state.racks.find(rack);
    if (rack_it == state.racks.end()) {
      result.status = CapacityAggregateStatus::IncompleteEvidence;
      result.reason = "member_rack_unknown";
      return result;
    }
    if (rack_it->second.reference.evidence.requires_revalidation()) {
      result.status = CapacityAggregateStatus::RevalidationRequired;
      result.reason = "member_rack_requires_revalidation";
      return result;
    }
  }

  if (any_unknown_value) {
    result.status = CapacityAggregateStatus::IncompleteEvidence;
    result.reason = "member_quantity_unknown";
    return result;
  }

  result.status = CapacityAggregateStatus::Known;
  result.value = total;
  result.provenance = contributing > 1 ? EvidenceProvenance::Derived : provenance;
  result.reason = contributing > 1 ? "derived_from_member_quantities" : "single_supplied_quantity";
  return result;
}

DomainIndependence failure_domain_independence(const ClusterState& state,
                                              const ClusterIndexes& indexes, const RackId& lhs,
                                              const RackId& rhs, FailureDomainClass klass) {
  if (klass == FailureDomainClass::Unknown) {
    return DomainIndependence::Unknown;
  }
  const auto lhs_it = state.racks.find(lhs);
  const auto rhs_it = state.racks.find(rhs);
  if (lhs_it == state.racks.end() || rhs_it == state.racks.end()) {
    return DomainIndependence::Unknown;
  }

  bool lhs_has_current = false;
  bool rhs_has_current = false;
  bool revalidation = false;
  bool shared = false;

  for (const FailureDomainId& id : indexes.failure_domains_of(lhs)) {
    const auto domain_it = state.failure_domains.find(id);
    if (domain_it == state.failure_domains.end()) {
      continue;
    }
    const FailureDomain& domain = domain_it->second;
    if (domain.klass != klass) {
      continue;
    }
    if (domain_needs_revalidation(domain.header)) {
      revalidation = true;
      continue;
    }
    if (!domain_is_current(domain.header)) {
      continue;
    }
    const bool in_lhs = std::binary_search(domain.racks.begin(), domain.racks.end(), lhs);
    const bool in_rhs = std::binary_search(domain.racks.begin(), domain.racks.end(), rhs);
    if (in_lhs) {
      lhs_has_current = true;
    }
    if (in_rhs) {
      rhs_has_current = true;
      if (in_lhs) {
        shared = true;
      }
    }
  }

  for (const FailureDomainId& id : indexes.failure_domains_of(rhs)) {
    const auto domain_it = state.failure_domains.find(id);
    if (domain_it == state.failure_domains.end()) {
      continue;
    }
    const FailureDomain& domain = domain_it->second;
    if (domain.klass != klass) {
      continue;
    }
    if (domain_needs_revalidation(domain.header)) {
      revalidation = true;
      continue;
    }
    if (!domain_is_current(domain.header)) {
      continue;
    }
    const bool in_lhs = std::binary_search(domain.racks.begin(), domain.racks.end(), lhs);
    const bool in_rhs = std::binary_search(domain.racks.begin(), domain.racks.end(), rhs);
    if (in_rhs) {
      rhs_has_current = true;
    }
    if (in_lhs) {
      lhs_has_current = true;
      if (in_rhs) {
        shared = true;
      }
    }
  }

  if (shared) {
    return DomainIndependence::Shared;
  }
  if (revalidation) {
    return DomainIndependence::RevalidationRequired;
  }
  if (lhs_has_current && rhs_has_current) {
    return DomainIndependence::Independent;
  }
  return DomainIndependence::Unknown;
}

Reachability reachability_between(const ClusterState& state, const RackId& source,
                                  const RackId& destination) {
  if (!state.has_rack(source) || !state.has_rack(destination)) {
    return Reachability::Unknown;
  }
  bool saw_current = false;
  bool saw_reachable = false;
  bool saw_unreachable = false;
  bool saw_revalidation = false;

  for (const auto& entry : state.links) {
    const InterRackLink& link = entry.second;
    bool forward = false;
    if (link.source == source && link.destination == destination) {
      forward = true;
    } else if (link.source == destination && link.destination == source &&
               link.direction == LinkDirection::Bidirectional) {
      forward = true;
    }
    if (!forward) {
      continue;
    }
    if (link.header.evidence.requires_revalidation()) {
      saw_revalidation = true;
      continue;
    }
    if (!link.header.evidence.is_current()) {
      continue;
    }
    saw_current = true;
    if (link.reachability == Reachability::Reachable) {
      saw_reachable = true;
    } else if (link.reachability == Reachability::Unreachable) {
      saw_unreachable = true;
    } else if (link.reachability == Reachability::RevalidationRequired) {
      saw_revalidation = true;
    }
  }

  if (saw_reachable) {
    return Reachability::Reachable;
  }
  if (saw_revalidation) {
    return Reachability::RevalidationRequired;
  }
  if (saw_current && saw_unreachable) {
    return Reachability::Unreachable;
  }
  return Reachability::Unknown;
}

// ---------------------------------------------------------------------------
// Readiness contract
// ---------------------------------------------------------------------------

ReadinessEvaluation evaluate_readiness(const ClusterState& state) {
  ReadinessEvaluation evaluation;

  if (state.lifecycle == ClusterLifecycle::Retired) {
    evaluation.lifecycle = ClusterLifecycle::Retired;
    evaluation.satisfied = false;
    add_blocker(evaluation, "cluster_retired", "cluster", "cluster is RETIRED");
    return evaluation;
  }
  if (state.lifecycle == ClusterLifecycle::Retiring) {
    evaluation.lifecycle = ClusterLifecycle::Retiring;
    evaluation.satisfied = false;
    add_blocker(evaluation, "cluster_retiring", "cluster", "cluster is RETIRING");
    return evaluation;
  }

  const ReadinessContract& contract = state.readiness_contract;

  std::size_t active_count = 0;
  std::size_t current_count = 0;
  bool any_revalidation = false;
  bool revalidation_is_required_element = false;

  for (const auto& entry : state.racks) {
    const RackRecord& record = entry.second;
    if (!is_active(record)) {
      continue;
    }
    ++active_count;
    if (is_current_active(record)) {
      ++current_count;
    }
    if (needs_revalidation(record)) {
      any_revalidation = true;
    }
  }

  if (active_count == 0) {
    add_blocker(evaluation, "no_active_rack", "cluster",
                "no rack is a member with membership ACTIVE");
  }
  if (current_count < contract.minimum_current_racks) {
    add_blocker(evaluation, "minimum_current_racks_not_met", "cluster",
                "current racks " + std::to_string(current_count) + " is below the required " +
                    std::to_string(contract.minimum_current_racks));
    if (any_revalidation) {
      revalidation_is_required_element = true;
    }
  }

  for (const RackId& mandatory : contract.mandatory_racks) {
    const auto it = state.racks.find(mandatory);
    if (it == state.racks.end()) {
      add_blocker(evaluation, "mandatory_rack_missing", "rack/" + mandatory.value(),
                  "mandatory rack is not a member of this cluster");
      continue;
    }
    if (!is_active(it->second)) {
      add_blocker(evaluation, "mandatory_rack_not_active", "rack/" + mandatory.value(),
                  "mandatory rack membership is " +
                      std::string(to_string(it->second.membership)));
      continue;
    }
    if (!is_current_active(it->second)) {
      add_blocker(evaluation, "mandatory_rack_not_current", "rack/" + mandatory.value(),
                  "mandatory rack is not authoritative current: " +
                      (it->second.non_authoritative_reason.empty()
                           ? std::string("unknown reason")
                           : it->second.non_authoritative_reason));
      revalidation_is_required_element = true;
    }
  }

  if (contract.require_all_active_racks_current) {
    for (const auto& entry : state.racks) {
      if (is_active(entry.second) && !entry.second.authoritative_current) {
        add_blocker(evaluation, "active_rack_not_current", "rack/" + entry.first.value(),
                    entry.second.non_authoritative_reason.empty()
                        ? std::string("active rack is not authoritative current")
                        : entry.second.non_authoritative_reason);
      }
    }
  }

  if (any_revalidation && !contract.allow_degraded) {
    revalidation_is_required_element = true;
  }
  if (any_revalidation) {
    for (const auto& entry : state.racks) {
      if (needs_revalidation(entry.second)) {
        add_blocker(evaluation, "rack_revalidation_required", "rack/" + entry.first.value(),
                    "dynamic evidence owned by a fenced or restarted authority must be "
                    "republished before the rack is current");
      }
    }
  }

  std::size_t current_links = 0;
  for (const auto& entry : state.links) {
    if (entry.second.header.evidence.is_current()) {
      ++current_links;
    }
  }
  if (contract.require_connectivity_evidence && current_links == 0) {
    add_blocker(evaluation, "connectivity_evidence_missing", "cluster",
                "no inter-rack link carries current evidence");
  }
  if (current_links < contract.minimum_current_links) {
    add_blocker(evaluation, "minimum_current_links_not_met", "cluster",
                "current links " + std::to_string(current_links) + " is below the required " +
                    std::to_string(contract.minimum_current_links));
  }

  for (PlacementDomainClass klass : contract.required_placement_domain_classes) {
    bool found = false;
    for (const auto& entry : state.placement_domains) {
      if (entry.second.klass == klass && domain_is_current(entry.second.header)) {
        found = true;
        break;
      }
    }
    if (!found) {
      add_blocker(evaluation, "placement_domain_class_missing", "placement_domain_class/" +
                                                                  std::string(to_string(klass)),
                  "no current placement domain of the required class");
    }
  }

  for (FailureDomainClass klass : contract.required_failure_domain_classes) {
    bool found = false;
    for (const auto& entry : state.failure_domains) {
      if (entry.second.klass == klass && domain_is_current(entry.second.header)) {
        found = true;
        break;
      }
    }
    if (!found) {
      add_blocker(evaluation, "failure_domain_class_missing",
                  "failure_domain_class/" + std::string(to_string(klass)),
                  "no current failure domain of the required class");
    }
  }

  if (contract.require_no_conflicts) {
    for (const auto& entry : state.links) {
      const InterRackLink& link = entry.second;
      if (!state.has_rack(link.source) || !state.has_rack(link.destination)) {
        add_blocker(evaluation, "structural_conflict", "link/" + entry.first.value(),
                    "link references a rack that is not a member of this cluster");
      }
    }
    for (const auto& entry : state.racks) {
      const RackRecord& record = entry.second;
      if (record.authoritative_current && state.is_boot_fenced(record.reference.boot)) {
        add_blocker(evaluation, "structural_conflict", "rack/" + entry.first.value(),
                    "rack is authoritative current under a fenced process incarnation");
      }
      if (record.authoritative_current && state.is_rack_retired(entry.first)) {
        add_blocker(evaluation, "structural_conflict", "rack/" + entry.first.value(),
                    "retired rack is still authoritative current");
      }
    }
  }

  std::sort(evaluation.blockers.begin(), evaluation.blockers.end(),
            [](const ReadinessBlocker& lhs, const ReadinessBlocker& rhs) {
              if (lhs.code != rhs.code) {
                return lhs.code < rhs.code;
              }
              return lhs.subject < rhs.subject;
            });

  evaluation.satisfied = evaluation.blockers.empty();

  if (revalidation_is_required_element) {
    evaluation.lifecycle = ClusterLifecycle::RevalidationRequired;
    return evaluation;
  }

  if (evaluation.satisfied) {
    bool knowingly_incomplete = false;
    for (const auto& entry : state.racks) {
      if (entry.second.membership == RackMembershipState::Unavailable ||
          entry.second.membership == RackMembershipState::Withdrawn) {
        knowingly_incomplete = true;
        break;
      }
    }
    if (!state.withdrawn_racks.empty()) {
      knowingly_incomplete = true;
    }
    if (knowingly_incomplete && contract.allow_partial) {
      evaluation.lifecycle = ClusterLifecycle::Partial;
    } else {
      evaluation.lifecycle = ClusterLifecycle::Ready;
    }
    return evaluation;
  }

  if (current_count > 0) {
    evaluation.lifecycle = ClusterLifecycle::Degraded;
    return evaluation;
  }
  if (active_count > 0) {
    evaluation.lifecycle = ClusterLifecycle::Forming;
    return evaluation;
  }
  evaluation.lifecycle = ClusterLifecycle::Declared;
  return evaluation;
}

}  // namespace cluster_fabric
