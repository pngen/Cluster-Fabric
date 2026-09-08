// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Immutable snapshots, semantic digests and currentness validation.

#include "cluster_fabric/snapshot.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cluster_fabric {

std::string_view to_string(SnapshotStaleReason value) noexcept {
  switch (value) {
    case SnapshotStaleReason::None: return "NONE";
    case SnapshotStaleReason::ClusterEpochAdvanced: return "CLUSTER_EPOCH_ADVANCED";
    case SnapshotStaleReason::CoordinatorEpochAdvanced: return "COORDINATOR_EPOCH_ADVANCED";
    case SnapshotStaleReason::ClusterGenerationAdvanced: return "CLUSTER_GENERATION_ADVANCED";
    case SnapshotStaleReason::MembershipChanged: return "MEMBERSHIP_CHANGED";
    case SnapshotStaleReason::TopologyEpochSuperseded: return "TOPOLOGY_EPOCH_SUPERSEDED";
    case SnapshotStaleReason::TopologyGenerationAdvanced: return "TOPOLOGY_GENERATION_ADVANCED";
    case SnapshotStaleReason::RackGenerationSuperseded: return "RACK_GENERATION_SUPERSEDED";
    case SnapshotStaleReason::RackWithdrawn: return "RACK_WITHDRAWN";
    case SnapshotStaleReason::RackRetired: return "RACK_RETIRED";
    case SnapshotStaleReason::RackBootFenced: return "RACK_BOOT_FENCED";
    case SnapshotStaleReason::RackRevalidationRequired: return "RACK_REVALIDATION_REQUIRED";
    case SnapshotStaleReason::PlacementDomainSuperseded: return "PLACEMENT_DOMAIN_SUPERSEDED";
    case SnapshotStaleReason::CapacityDomainSuperseded: return "CAPACITY_DOMAIN_SUPERSEDED";
    case SnapshotStaleReason::FailureDomainSuperseded: return "FAILURE_DOMAIN_SUPERSEDED";
    case SnapshotStaleReason::NetworkDomainSuperseded: return "NETWORK_DOMAIN_SUPERSEDED";
    case SnapshotStaleReason::StorageDomainSuperseded: return "STORAGE_DOMAIN_SUPERSEDED";
    case SnapshotStaleReason::PowerDomainSuperseded: return "POWER_DOMAIN_SUPERSEDED";
    case SnapshotStaleReason::CoolingDomainSuperseded: return "COOLING_DOMAIN_SUPERSEDED";
    case SnapshotStaleReason::LinkDomainSuperseded: return "LINK_DOMAIN_SUPERSEDED";
    case SnapshotStaleReason::ConnectivitySuperseded: return "CONNECTIVITY_SUPERSEDED";
    case SnapshotStaleReason::HealthSuperseded: return "HEALTH_SUPERSEDED";
    case SnapshotStaleReason::LifecycleNotConsumable: return "LIFECYCLE_NOT_CONSUMABLE";
    case SnapshotStaleReason::WrongCluster: return "WRONG_CLUSTER";
  }
  return "NONE";
}

namespace {

/// FNV-1a 64 over a deterministic rendering. Used for the semantic digest.
class DigestBuilder {
 public:
  void text(std::string_view value) {
    for (char c : value) {
      hash_ ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
      hash_ *= 1099511628211ull;
    }
    separator();
  }

  void number(std::uint64_t value) { text(std::to_string(value)); }

  [[nodiscard]] std::uint64_t value() const noexcept { return hash_; }

 private:
  void separator() {
    hash_ ^= 0x7cull;
    hash_ *= 1099511628211ull;
  }

  std::uint64_t hash_ = 1469598103934665603ull;
};

[[nodiscard]] std::string to_hex(std::uint64_t value) {
  static const char kHex[] = "0123456789abcdef";
  std::string out(16, '0');
  for (std::size_t i = 0; i < 16; ++i) {
    out[15 - i] = kHex[(value >> (4 * i)) & 0xfull];
  }
  return out;
}

[[nodiscard]] std::string generation_pair(const DomainHeader& header) {
  return "g" + header.generation.str() + "/p" +
         std::string(to_string(header.evidence.provenance)) + "/f" +
         std::string(to_string(header.evidence.freshness)) + "/e" +
         std::to_string(header.evidence.observed_at.millis());
}

void digest_domain(DigestBuilder& builder, const DomainHeader& header) {
  builder.text(generation_pair(header));
  builder.text(header.label);
}

void add_reason(SnapshotValidation& validation, SnapshotStaleReason reason, std::string subject) {
  validation.reasons.push_back(reason);
  validation.subjects.push_back(std::move(subject));
}

}  // namespace

std::string semantic_digest_of(const ClusterState& state) {
  DigestBuilder builder;
  builder.text("cluster_fabric/snapshot/v1");
  builder.text(state.id.view());
  builder.number(state.epoch.value());
  builder.number(state.coordinator_epoch.value());
  builder.number(state.generation.value());
  builder.number(state.membership_generation.value());
  builder.number(state.topology_epoch.value());
  builder.number(state.topology_generation.value());
  builder.number(state.connectivity_generation.value());
  builder.number(state.health_generation.value());
  builder.number(state.constraint_generation.value());
  builder.number(state.domain_generations.placement.value());
  builder.number(state.domain_generations.capacity.value());
  builder.number(state.domain_generations.failure.value());
  builder.number(state.domain_generations.network.value());
  builder.number(state.domain_generations.storage.value());
  builder.number(state.domain_generations.power.value());
  builder.number(state.domain_generations.cooling.value());
  builder.number(state.domain_generations.link.value());
  builder.number(state.snapshot_generation.value());
  builder.number(state.publication_generation.value());
  builder.number(static_cast<std::uint64_t>(state.lifecycle));
  builder.text(state.topology_record.reason);
  builder.number(state.topology_record.established_at.millis());

  for (const auto& entry : state.racks) {
    const RackRecord& record = entry.second;
    builder.text("rack");
    builder.text(entry.first.view());
    builder.number(record.reference.generation.value());
    builder.number(static_cast<std::uint64_t>(record.membership));
    builder.number(static_cast<std::uint64_t>(record.reference.rack_lifecycle));
    builder.number(static_cast<std::uint64_t>(record.reference.currentness));
    builder.number(static_cast<std::uint64_t>(record.reference.health));
    builder.number(record.authoritative_current ? 1 : 0);
    builder.number(static_cast<std::uint64_t>(record.reference.evidence.provenance));
    builder.number(static_cast<std::uint64_t>(record.reference.evidence.freshness));
    builder.number(record.reference.evidence.observed_at.millis());
    builder.text(record.reference.composition.composition_label);
    for (const AcceleratorClassSummary& summary : record.reference.composition.accelerators) {
      builder.number(static_cast<std::uint64_t>(summary.vendor));
      builder.text(summary.family);
      builder.number(summary.device_count.has_value() ? *summary.device_count : 0);
      builder.number(summary.device_count.has_value() ? 1 : 0);
    }
    for (const RackEndpoint& endpoint : record.reference.endpoints) {
      builder.text(endpoint.id.view());
      builder.number(static_cast<std::uint64_t>(endpoint.connectivity));
    }
    for (const RackFailureDomainHint& hint : record.reference.failure_domain_hints) {
      builder.number(static_cast<std::uint64_t>(hint.klass));
      builder.text(hint.id.view());
    }
  }

  for (const auto& entry : state.links) {
    const InterRackLink& link = entry.second;
    builder.text("link");
    builder.text(entry.first.view());
    builder.text(link.source.view());
    builder.text(link.destination.view());
    builder.number(static_cast<std::uint64_t>(link.direction));
    builder.number(static_cast<std::uint64_t>(link.connectivity));
    builder.number(static_cast<std::uint64_t>(link.reachability));
    builder.number(static_cast<std::uint64_t>(link.health));
    builder.number(static_cast<std::uint64_t>(link.bandwidth_class));
    builder.number(link.nominal_bandwidth_bps.has_value() ? *link.nominal_bandwidth_bps : 0);
    builder.number(link.nominal_bandwidth_bps.has_value() ? 1 : 0);
    builder.number(static_cast<std::uint64_t>(link.latency_class));
    builder.number(link.nominal_latency_nanos.has_value() ? *link.nominal_latency_nanos : 0);
    builder.number(link.nominal_latency_nanos.has_value() ? 1 : 0);
    builder.number(link.topology_epoch.value());
    builder.number(link.topology_generation.value());
    digest_domain(builder, link.header);
    for (const FailureDomainId& domain : link.failure_domains) {
      builder.text(domain.view());
    }
  }

  auto digest_placement = [&builder](const std::map<PlacementDomainId, PlacementDomain>& domains) {
    for (const auto& entry : domains) {
      builder.text("placement_domain");
      builder.text(entry.first.view());
      builder.number(static_cast<std::uint64_t>(entry.second.klass));
      for (const RackId& rack : entry.second.racks) {
        builder.text(rack.view());
      }
      digest_domain(builder, entry.second.header);
    }
  };
  digest_placement(state.placement_domains);

  for (const auto& entry : state.capacity_domains) {
    builder.text("capacity_domain");
    builder.text(entry.first.view());
    builder.number(static_cast<std::uint64_t>(entry.second.klass));
    for (const RackId& rack : entry.second.racks) {
      builder.text(rack.view());
    }
    for (const CapacityQuantity& quantity : entry.second.quantities) {
      builder.text(quantity.unit);
      builder.number(quantity.value.has_value() ? 1 : 0);
      if (quantity.value.has_value()) {
        builder.text(std::to_string(*quantity.value));
      }
      builder.number(static_cast<std::uint64_t>(quantity.provenance));
      builder.number(quantity.aggregated ? 1 : 0);
    }
    digest_domain(builder, entry.second.header);
  }

  for (const auto& entry : state.failure_domains) {
    builder.text("failure_domain");
    builder.text(entry.first.view());
    builder.number(static_cast<std::uint64_t>(entry.second.klass));
    for (const RackId& rack : entry.second.racks) {
      builder.text(rack.view());
    }
    digest_domain(builder, entry.second.header);
  }

  for (const auto& entry : state.network_domains) {
    builder.text("network_domain");
    builder.text(entry.first.view());
    builder.number(static_cast<std::uint64_t>(entry.second.connectivity));
    for (const RackId& rack : entry.second.racks) {
      builder.text(rack.view());
    }
    digest_domain(builder, entry.second.header);
  }

  for (const auto& entry : state.storage_domains) {
    builder.text("storage_domain");
    builder.text(entry.first.view());
    for (const RackId& rack : entry.second.racks) {
      builder.text(rack.view());
    }
    digest_domain(builder, entry.second.header);
  }

  for (const auto& entry : state.power_domains) {
    builder.text("power_domain");
    builder.text(entry.first.view());
    for (const RackId& rack : entry.second.racks) {
      builder.text(rack.view());
    }
    builder.text(entry.second.parent.has_value() ? entry.second.parent->view() : std::string_view{});
    digest_domain(builder, entry.second.header);
  }

  for (const auto& entry : state.cooling_domains) {
    builder.text("cooling_domain");
    builder.text(entry.first.view());
    for (const RackId& rack : entry.second.racks) {
      builder.text(rack.view());
    }
    builder.text(entry.second.parent.has_value() ? entry.second.parent->view() : std::string_view{});
    digest_domain(builder, entry.second.header);
  }

  for (const auto& entry : state.link_domains) {
    builder.text("link_domain");
    builder.text(entry.first.view());
    for (const RackId& rack : entry.second.racks) {
      builder.text(rack.view());
    }
    digest_domain(builder, entry.second.header);
  }

  for (const auto& entry : state.constraints) {
    builder.text("constraint");
    builder.text(entry.first.view());
    builder.number(static_cast<std::uint64_t>(entry.second.kind));
    builder.text(entry.second.domain_ref);
    builder.text(entry.second.statement);
    for (const RackId& rack : entry.second.racks) {
      builder.text(rack.view());
    }
    digest_domain(builder, entry.second.header);
  }

  for (const RetiredIdentity& retired : state.retired_racks) {
    builder.text("retired");
    builder.text(retired.rack.view());
    builder.number(retired.last_generation.value());
  }
  for (const RackId& rack : state.withdrawn_racks) {
    builder.text("withdrawn");
    builder.text(rack.view());
  }
  for (const FencedAuthority& fenced : state.fenced_authorities) {
    builder.text("fenced");
    builder.text(fenced.boot.view());
    builder.text(fenced.rack.view());
    builder.text(fenced.reason);
  }
  return to_hex(builder.value());
}

void ClusterSnapshot::rebuild_bindings() {
  bindings_.clear();
  if (state_ == nullptr) {
    return;
  }
  bindings_.reserve(state_->racks.size());
  for (const auto& entry : state_->racks) {
    RackGenerationBinding binding;
    binding.rack = entry.first;
    binding.generation = entry.second.reference.generation;
    binding.membership = entry.second.membership;
    binding.currentness = entry.second.reference.currentness;
    binding.authoritative_current = entry.second.authoritative_current;
    binding.boot = entry.second.reference.boot;
    bindings_.push_back(std::move(binding));
  }
}

ClusterSnapshot ClusterSnapshot::capture(const ClusterState& state) {
  ClusterSnapshot snapshot;
  snapshot.state_ = std::make_shared<const ClusterState>(state);
  snapshot.rebuild_bindings();
  snapshot.digest_ = semantic_digest_of(*snapshot.state_);
  return snapshot;
}

std::optional<RackGeneration> ClusterSnapshot::rack_generation(const RackId& rack) const noexcept {
  if (state_ == nullptr) {
    return std::nullopt;
  }
  const auto it = state_->racks.find(rack);
  if (it == state_->racks.end()) {
    return std::nullopt;
  }
  return it->second.reference.generation;
}

bool SnapshotValidation::requires_revalidation() const noexcept {
  for (SnapshotStaleReason reason : reasons) {
    switch (reason) {
      case SnapshotStaleReason::ClusterEpochAdvanced:
      case SnapshotStaleReason::CoordinatorEpochAdvanced:
      case SnapshotStaleReason::RackBootFenced:
      case SnapshotStaleReason::RackRevalidationRequired:
      case SnapshotStaleReason::LifecycleNotConsumable:
        return true;
      default:
        break;
    }
  }
  return false;
}

std::string SnapshotValidation::describe() const {
  std::string out = current ? "snapshot=current" : "snapshot=stale";
  out += consumable ? " consumable=true" : " consumable=false";
  out += " reasons=" + std::to_string(reasons.size());
  for (std::size_t i = 0; i < reasons.size(); ++i) {
    out += " ";
    out += std::string(to_string(reasons[i]));
    if (i < subjects.size() && !subjects[i].empty()) {
      out += "(";
      out += subjects[i];
      out += ")";
    }
  }
  return out;
}

SnapshotValidation ClusterSnapshot::validate(const ClusterState& current) const {
  SnapshotValidation validation;
  if (state_ == nullptr) {
    validation.current = false;
    validation.consumable = false;
    validation.explanation =
        Explanation::make("snapshot_invalid", "snapshot", "the snapshot holds no state");
    return validation;
  }

  const ClusterState& bound = *state_;

  if (bound.id != current.id) {
    add_reason(validation, SnapshotStaleReason::WrongCluster,
               "snapshot/" + bound.id.value() + " current/" + current.id.value());
  }
  if (bound.epoch != current.epoch) {
    add_reason(validation, SnapshotStaleReason::ClusterEpochAdvanced,
               "bound=" + bound.epoch.str() + " current=" + current.epoch.str());
  }
  if (bound.coordinator_epoch != current.coordinator_epoch) {
    add_reason(validation, SnapshotStaleReason::CoordinatorEpochAdvanced,
               "bound=" + bound.coordinator_epoch.str() +
                   " current=" + current.coordinator_epoch.str());
  }
  if (bound.generation != current.generation) {
    add_reason(validation, SnapshotStaleReason::ClusterGenerationAdvanced,
               "bound=" + bound.generation.str() + " current=" + current.generation.str());
  }
  if (bound.membership_generation != current.membership_generation ||
      bound.racks.size() != current.racks.size()) {
    add_reason(validation, SnapshotStaleReason::MembershipChanged,
               "bound=" + bound.membership_generation.str() +
                   " current=" + current.membership_generation.str());
  }
  if (bound.topology_epoch != current.topology_epoch) {
    add_reason(validation, SnapshotStaleReason::TopologyEpochSuperseded,
               "bound=" + bound.topology_epoch.str() + " current=" + current.topology_epoch.str());
  } else if (bound.topology_generation != current.topology_generation) {
    add_reason(validation, SnapshotStaleReason::TopologyGenerationAdvanced,
               "bound=" + bound.topology_generation.str() +
                   " current=" + current.topology_generation.str());
  }
  if (bound.domain_generations.placement != current.domain_generations.placement) {
    add_reason(validation, SnapshotStaleReason::PlacementDomainSuperseded, "placement_domain");
  }
  if (bound.domain_generations.capacity != current.domain_generations.capacity) {
    add_reason(validation, SnapshotStaleReason::CapacityDomainSuperseded, "capacity_domain");
  }
  if (bound.domain_generations.failure != current.domain_generations.failure) {
    add_reason(validation, SnapshotStaleReason::FailureDomainSuperseded, "failure_domain");
  }
  if (bound.domain_generations.network != current.domain_generations.network) {
    add_reason(validation, SnapshotStaleReason::NetworkDomainSuperseded, "network_domain");
  }
  if (bound.domain_generations.storage != current.domain_generations.storage) {
    add_reason(validation, SnapshotStaleReason::StorageDomainSuperseded, "storage_domain");
  }
  if (bound.domain_generations.power != current.domain_generations.power) {
    add_reason(validation, SnapshotStaleReason::PowerDomainSuperseded, "power_domain");
  }
  if (bound.domain_generations.cooling != current.domain_generations.cooling) {
    add_reason(validation, SnapshotStaleReason::CoolingDomainSuperseded, "cooling_domain");
  }
  if (bound.domain_generations.link != current.domain_generations.link) {
    add_reason(validation, SnapshotStaleReason::LinkDomainSuperseded, "link_domain");
  }
  if (bound.connectivity_generation != current.connectivity_generation) {
    add_reason(validation, SnapshotStaleReason::ConnectivitySuperseded,
               "bound=" + bound.connectivity_generation.str() +
                   " current=" + current.connectivity_generation.str());
  }
  if (bound.health_generation != current.health_generation) {
    add_reason(validation, SnapshotStaleReason::HealthSuperseded,
               "bound=" + bound.health_generation.str() +
                   " current=" + current.health_generation.str());
  }

  for (const RackGenerationBinding& binding : bindings_) {
    const auto it = current.racks.find(binding.rack);
    if (it == current.racks.end()) {
      add_reason(validation, SnapshotStaleReason::RackWithdrawn,
                 "rack/" + binding.rack.value() + " is no longer a member");
      continue;
    }
    const RackRecord& record = it->second;
    if (current.is_rack_retired(binding.rack)) {
      add_reason(validation, SnapshotStaleReason::RackRetired, "rack/" + binding.rack.value());
      continue;
    }
    if (record.reference.generation != binding.generation) {
      add_reason(validation, SnapshotStaleReason::RackGenerationSuperseded,
                 "rack/" + binding.rack.value() + " bound=" + binding.generation.str() +
                     " current=" + record.reference.generation.str());
    }
    if (record.membership != binding.membership) {
      add_reason(validation, SnapshotStaleReason::RackWithdrawn,
                 "rack/" + binding.rack.value() + " membership=" +
                     std::string(to_string(record.membership)));
    }
    if (current.is_boot_fenced(binding.boot)) {
      add_reason(validation, SnapshotStaleReason::RackBootFenced,
                 "rack/" + binding.rack.value() + " boot=" + binding.boot.value());
    }
    if (record.reference.currentness == RackCurrentness::RevalidationRequired ||
        record.reference.evidence.requires_revalidation()) {
      add_reason(validation, SnapshotStaleReason::RackRevalidationRequired,
                 "rack/" + binding.rack.value());
    }
  }

  for (const auto& entry : current.racks) {
    if (std::none_of(bindings_.begin(), bindings_.end(),
                     [&entry](const RackGenerationBinding& binding) {
                       return binding.rack == entry.first;
                     })) {
      add_reason(validation, SnapshotStaleReason::MembershipChanged,
                 "rack/" + entry.first.value() + " was added after the snapshot");
    }
  }

  validation.current = validation.reasons.empty();
  validation.consumable = validation.current && is_consumable_lifecycle(current.lifecycle);
  if (!is_consumable_lifecycle(current.lifecycle)) {
    // The generations match, but the cluster is not in a state that may be
    // consumed. This is a typed reason, not a generation mismatch.
    add_reason(validation, SnapshotStaleReason::LifecycleNotConsumable,
               std::string(to_string(current.lifecycle)));
  }

  validation.explanation =
      Explanation::make(validation.current ? "snapshot_current" : "snapshot_stale", "snapshot",
                        validation.describe());
  for (std::size_t i = 0; i < validation.reasons.size(); ++i) {
    validation.explanation.add(std::string(to_string(validation.reasons[i])),
                               i < validation.subjects.size() ? validation.subjects[i]
                                                              : std::string(),
                               "bound snapshot no longer matches current authority");
  }
  validation.explanation.sort_factors();
  validation.explanation.bound_factors();
  return validation;
}

SnapshotValidation ClusterSnapshot::validate(const ClusterSnapshot& current) const {
  if (current.state_ == nullptr) {
    SnapshotValidation validation;
    validation.explanation =
        Explanation::make("snapshot_invalid", "snapshot", "the comparison snapshot is empty");
    return validation;
  }
  return validate(*current.state_);
}

}  // namespace cluster_fabric
