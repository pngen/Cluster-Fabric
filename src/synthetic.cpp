// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic synthetic cluster laboratory.
//
// Every request produced here is reproducible from SyntheticConfig alone. The
// random stream is a SplitMix64 generator seeded only by config.seed; no wall
// clock, thread, process identity, address or locale influences the output, so
// an identical configuration always encodes to identical bytes. Every evidence
// stamp is SYNTHETIC and is never presented as physical.
//
// The sequence follows the order a coordinator accepts: declare the cluster,
// register each rack publisher, admit racks, publish the descriptive domains,
// supersede the topology epoch when the scenario asks for it, publish the
// inter-rack links under the resulting epoch, publish the cluster constraint,
// then advance rack generations. Topology supersession precedes link
// publication because a link is stamped with the topology epoch in force when
// it is published; publishing a supersession while links exist leaves those
// links carrying a superseded epoch.
//
// Every count is clamped against limits.hpp before anything is reserved, so an
// absurd configuration yields a bounded scenario instead of an allocation
// failure, and no loop is bounded by anything other than a clamped count.

#include "cluster_fabric/synthetic.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cluster_fabric/evidence.hpp"
#include "cluster_fabric/generation.hpp"
#include "cluster_fabric/limits.hpp"
#include "cluster_fabric/lifecycle.hpp"
#include "cluster_fabric/taxonomy.hpp"

namespace cluster_fabric {
namespace {

// ---------------------------------------------------------------------------
// Deterministic random stream
// ---------------------------------------------------------------------------

/// SplitMix64. Small, fast and fully specified, so a given seed always yields
/// the same stream on every platform and build.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

  /// Uniform value in [0, bound). Returns zero when bound is zero.
  [[nodiscard]] std::uint64_t below(std::uint64_t bound) noexcept {
    if (bound == 0) {
      return 0;
    }
    return next() % bound;
  }

 private:
  std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Fixed constants
// ---------------------------------------------------------------------------

/// Deterministic evidence epoch. Synthetic evidence never uses the wall clock.
inline constexpr std::int64_t kStampBaseMillis = 1'700'000'000'000LL;
inline constexpr std::int64_t kStampStepMillis = 1'000LL;
inline constexpr std::int64_t kStampTtlMillis = 30'000LL;
inline constexpr const char* kOriginLabel = "rack-fabric:synthetic:1.0.0";
inline constexpr const char* kRequestNote = "synthetic-cluster-generation";
inline constexpr const char* kTopologyReason = "synthetic_topology_epoch_change";

/// Upper bound on generated topology supersessions. Keeps an absurd count from
/// producing an unbounded request sequence.
inline constexpr std::size_t kMaxTopologyEpochChanges = 1024;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

/// Parses a generated identity. The generated alphabet is always accepted, so
/// the fallback is unreachable for every value this file produces.
template <class Id>
[[nodiscard]] Id make_id(std::string_view text) {
  const auto parsed = Id::parse(text);
  return parsed.has_value() ? *parsed : Id{};
}

[[nodiscard]] std::string lowercase(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

[[nodiscard]] EvidenceStamp synthetic_stamp(std::int64_t ordinal) noexcept {
  const std::int64_t offset = ordinal < 0 ? 0 : ordinal;
  return EvidenceStamp::make(EvidenceProvenance::Synthetic,
                             Timestamp::from_unix_millis(kStampBaseMillis + offset * kStampStepMillis),
                             kStampTtlMillis);
}

/// Locale-independent fixed-point rendering with six fractional digits.
[[nodiscard]] std::string fixed_six(double value) {
  if (!(value == value)) {
    return "nan";
  }
  const double scaled = value * 1000000.0;
  if (!(scaled >= -9.0e15 && scaled <= 9.0e15)) {
    return value > 0.0 ? "large-positive" : "large-negative";
  }
  const std::int64_t rounded =
      static_cast<std::int64_t>(scaled >= 0.0 ? scaled + 0.5 : scaled - 0.5);
  const bool negative = rounded < 0;
  const std::uint64_t magnitude =
      negative ? static_cast<std::uint64_t>(-rounded) : static_cast<std::uint64_t>(rounded);
  std::string digits = std::to_string(magnitude);
  while (digits.size() <= 6) {
    digits.insert(digits.begin(), '0');
  }
  std::string out;
  if (negative) {
    out.push_back('-');
  }
  out.append(digits, 0, digits.size() - 6);
  out.push_back('.');
  out.append(digits, digits.size() - 6, 6);
  return out;
}

[[nodiscard]] std::string join_indices(const std::vector<std::size_t>& values) {
  if (values.empty()) {
    return "none";
  }
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    out += std::to_string(values[i]);
  }
  return out;
}

/// Deterministic subset selection: a bounded partial Fisher-Yates draw.
[[nodiscard]] std::vector<std::size_t> select_subset(std::vector<std::size_t> candidates,
                                                     std::size_t count, SplitMix64& rng) {
  if (count >= candidates.size()) {
    return candidates;
  }
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t span = candidates.size() - i;
    const std::size_t offset = static_cast<std::size_t>(rng.below(static_cast<std::uint64_t>(span)));
    std::swap(candidates[i], candidates[i + offset]);
  }
  candidates.resize(count);
  return candidates;
}

/// Effective group size so that p item_count items fit into p limit groups.
[[nodiscard]] std::size_t effective_group_size(std::size_t requested, std::size_t item_count,
                                               std::size_t limit) noexcept {
  std::size_t group = requested == 0 ? 1 : requested;
  if (item_count == 0 || limit == 0) {
    return group;
  }
  const std::size_t minimum = (item_count + limit - 1) / limit;
  if (minimum > group) {
    group = minimum;
  }
  return group;
}

[[nodiscard]] std::size_t group_count(std::size_t item_count, std::size_t group) noexcept {
  if (item_count == 0 || group == 0) {
    return 0;
  }
  return (item_count + group - 1) / group;
}

/// Domain member lists are canonical: sorted by identity and deduplicated.
void sort_unique_racks(std::vector<RackId>& racks) {
  std::sort(racks.begin(), racks.end());
  racks.erase(std::unique(racks.begin(), racks.end()), racks.end());
}

[[nodiscard]] std::vector<RackId> member_slice(const std::vector<std::size_t>& included,
                                               std::size_t begin, std::size_t end) {
  std::vector<RackId> members;
  if (begin >= included.size() || begin >= end) {
    return members;
  }
  const std::size_t stop = (std::min)(end, included.size());
  members.reserve(stop - begin);
  for (std::size_t i = begin; i < stop; ++i) {
    members.push_back(synthetic_rack_id(included[i]));
  }
  return members;
}

[[nodiscard]] RackEndpointId endpoint_id(std::size_t index) {
  return make_id<RackEndpointId>("ep-syn-" + std::to_string(index));
}

// ---------------------------------------------------------------------------
// Composition
// ---------------------------------------------------------------------------

[[nodiscard]] AcceleratorClassSummary make_accelerator(std::size_t vendor_index,
                                                       std::size_t device_index) {
  static const AcceleratorVendor kVendors[] = {AcceleratorVendor::Nvidia, AcceleratorVendor::Amd,
                                               AcceleratorVendor::Intel, AcceleratorVendor::Other};
  static const char* kFamilies[] = {"H100", "MI300X", "Gaudi3", "SyntheticAccelerator"};
  static const std::uint32_t kDeviceCounts[] = {4u, 8u, 16u, 32u};
  static const std::uint64_t kMemoryPerDevice[] = {40ull, 80ull, 96ull, 128ull};

  AcceleratorClassSummary summary;
  summary.vendor = kVendors[vendor_index % 4];
  summary.family = kFamilies[vendor_index % 4];
  summary.device_count = kDeviceCounts[device_index % 4];
  summary.device_memory_bytes =
      kMemoryPerDevice[device_index % 4] * static_cast<std::uint64_t>(kDeviceCounts[device_index % 4]) *
      (1ull << 30);
  return summary;
}

[[nodiscard]] RackCompositionSummary make_composition(std::size_t index, bool heterogeneous,
                                                      SplitMix64& rng) {
  RackCompositionSummary summary;
  std::size_t vendor_index = 0;
  std::size_t device_index = 1;
  if (heterogeneous) {
    vendor_index = static_cast<std::size_t>(rng.below(4));
    device_index = static_cast<std::size_t>(rng.below(4));
  }
  summary.accelerators.push_back(make_accelerator(vendor_index, device_index));
  if (heterogeneous && (index % 3) == 2) {
    summary.accelerators.push_back(make_accelerator(vendor_index + 1, device_index + 1));
  }
  std::sort(summary.accelerators.begin(), summary.accelerators.end());
  summary.cpu_sockets = 2u;
  summary.cpu_cores = 96u + static_cast<std::uint32_t>(index % 5) * 32u;
  summary.host_memory_bytes = (512ull + static_cast<std::uint64_t>(index % 7) * 128ull) << 30;
  summary.nic_count = 2u + static_cast<std::uint32_t>(index % 3) * 2u;
  summary.switch_count = 2u;
  summary.composition_label = "synthetic-gpu-dense";
  summary.provenance = EvidenceProvenance::Synthetic;
  return summary;
}

// ---------------------------------------------------------------------------
// Scenario assembly
// ---------------------------------------------------------------------------

/// Everything the generator tracks while it emits requests, including the
/// shadow authority that must match the coordinator after each accepted step.
struct Scenario {
  const SyntheticConfig& config;
  SplitMix64 rng;
  SyntheticCluster cluster;

  ClusterEpoch cluster_epoch = ClusterEpoch::from_raw(1);
  CoordinatorEpoch coordinator_epoch = CoordinatorEpoch::from_raw(1);
  TopologyEpoch topology_epoch = TopologyEpoch::from_raw(1);
  TopologyGeneration topology_generation = TopologyGeneration::from_raw(1);
  PublicationGeneration publication = PublicationGeneration::from_raw(1);

  std::vector<RackCompositionSummary> compositions;
  std::vector<std::uint64_t> rack_publications;
  std::size_t stamp_ordinal = 0;

  explicit Scenario(const SyntheticConfig& scenario_config)
      : config(scenario_config), rng(scenario_config.seed) {}

  [[nodiscard]] std::int64_t next_stamp_ordinal() noexcept {
    ++stamp_ordinal;
    return static_cast<std::int64_t>(stamp_ordinal);
  }

  [[nodiscard]] MutationRequest base_request(MutationKind kind) const {
    MutationRequest request;
    request.kind = kind;
    request.cluster = config.cluster;
    request.authority.cluster_epoch = cluster_epoch;
    request.authority.coordinator_epoch = coordinator_epoch;
    request.authority.topology_epoch = topology_epoch;
    request.authority.topology_generation = topology_generation;
    request.authority.publication = publication;
    request.reason = kRequestNote;
    return request;
  }

  [[nodiscard]] DomainHeader make_header(std::size_t generation, std::string label,
                                         std::int64_t ordinal) const {
    DomainHeader header;
    header.evidence = synthetic_stamp(ordinal);
    header.generation = DomainGeneration::from_raw(generation);
    if (config.publisher.known()) {
      header.publisher = config.publisher;
    }
    header.cluster_epoch = cluster_epoch;
    header.coordinator_epoch = coordinator_epoch;
    header.label = std::move(label);
    return header;
  }

  [[nodiscard]] RackReference make_reference(std::size_t index, RackGeneration generation,
                                             std::uint64_t publication_value) const {
    RackReference reference;
    reference.rack = synthetic_rack_id(index);
    reference.generation = generation;
    reference.rack_lifecycle = RackLifecycleState::Ready;
    reference.currentness = RackCurrentness::Unknown;
    if (index < compositions.size()) {
      reference.composition = compositions[index];
    }
    RackEndpoint endpoint;
    endpoint.id = endpoint_id(index);
    endpoint.connectivity = ConnectivityClass::DirectFabric;
    endpoint.evidence = synthetic_stamp(static_cast<std::int64_t>(index));
    reference.endpoints.push_back(std::move(endpoint));
    RackFailureDomainHint hint;
    hint.klass = FailureDomainClass::Row;
    hint.id = synthetic_failure_domain_id(FailureDomainClass::Row, index);
    hint.evidence = synthetic_stamp(static_cast<std::int64_t>(index));
    reference.failure_domain_hints.push_back(std::move(hint));
    reference.evidence = synthetic_stamp(static_cast<std::int64_t>(index));
    if (config.publisher.known()) {
      reference.publisher = config.publisher;
    }
    reference.publication = PublicationGeneration::from_raw(publication_value);
    if (config.boot.known()) {
      reference.boot = config.boot;
    }
    reference.cluster_epoch = cluster_epoch;
    reference.coordinator_epoch = coordinator_epoch;
    reference.health = HealthState::Healthy;
    reference.origin_label = kOriginLabel;
    return reference;
  }

  void push(MutationRequest request) { cluster.requests.push_back(std::move(request)); }
};

[[nodiscard]] std::vector<RackId> rack_ids_of(const std::vector<std::size_t>& indices) {
  std::vector<RackId> ids;
  ids.reserve(indices.size());
  for (std::size_t index : indices) {
    ids.push_back(synthetic_rack_id(index));
  }
  // Ordered by identity, not by generation index: identity ordering is what a
  // caller may rely on (binary search), and indices order differently once a
  // cluster has more than nine racks.
  std::sort(ids.begin(), ids.end());
  return ids;
}

}  // namespace

// ---------------------------------------------------------------------------
// Identity helpers
// ---------------------------------------------------------------------------

[[nodiscard]] RackId synthetic_rack_id(std::size_t index) {
  return make_id<RackId>("rack-syn-" + std::to_string(index));
}

[[nodiscard]] PlacementDomainId synthetic_placement_domain_id(std::size_t index) {
  return make_id<PlacementDomainId>("pd-syn-" + std::to_string(index));
}

[[nodiscard]] FailureDomainId synthetic_failure_domain_id(FailureDomainClass klass,
                                                          std::size_t index) {
  return make_id<FailureDomainId>("fd-" + lowercase(to_string(klass)) + "-" +
                                  std::to_string(index));
}

[[nodiscard]] CapacityDomainId synthetic_capacity_domain_id(std::size_t index) {
  return make_id<CapacityDomainId>("cd-syn-" + std::to_string(index));
}

[[nodiscard]] NetworkDomainId synthetic_network_domain_id(std::size_t index) {
  return make_id<NetworkDomainId>("nd-syn-" + std::to_string(index));
}

[[nodiscard]] InterRackLinkId synthetic_link_id(const RackId& source,
                                                const RackId& destination) {
  return make_id<InterRackLinkId>("link-" + source.value() + "-" + destination.value());
}

// ---------------------------------------------------------------------------
// Reproduction parameters
// ---------------------------------------------------------------------------

[[nodiscard]] std::string SyntheticConfig::reproduction_parameters() const {
  std::vector<std::pair<std::string, std::string>> fields;
  fields.reserve(24);
  const auto identity_text = [](const auto& id) -> std::string {
    return id.known() ? id.value() : std::string("<unknown>");
  };
  fields.emplace_back("seed", std::to_string(seed));
  fields.emplace_back("cluster", identity_text(cluster));
  fields.emplace_back("publisher", identity_text(publisher));
  fields.emplace_back("boot", identity_text(boot));
  fields.emplace_back("rack_count", std::to_string(rack_count));
  fields.emplace_back("racks_per_placement_domain", std::to_string(racks_per_placement_domain));
  fields.emplace_back("racks_per_failure_domain", std::to_string(racks_per_failure_domain));
  fields.emplace_back("network_domain_count", std::to_string(network_domain_count));
  fields.emplace_back("link_density", fixed_six(link_density));
  fields.emplace_back("heterogeneous", heterogeneous ? "true" : "false");
  fields.emplace_back("missing_rack_count", std::to_string(missing_rack_count));
  fields.emplace_back("stale_rack_count", std::to_string(stale_rack_count));
  fields.emplace_back("rack_generation_changes", std::to_string(rack_generation_changes));
  fields.emplace_back("degraded_domain_count", std::to_string(degraded_domain_count));
  fields.emplace_back("partition_count", std::to_string(partition_count));
  fields.emplace_back("asymmetric_connectivity", asymmetric_connectivity ? "true" : "false");
  fields.emplace_back("topology_epoch_changes", std::to_string(topology_epoch_changes));
  fields.emplace_back("include_power_cooling", include_power_cooling ? "true" : "false");
  fields.emplace_back("mandatory_rack_indices", join_indices(mandatory_rack_indices));
  std::sort(fields.begin(), fields.end(),
            [](const std::pair<std::string, std::string>& lhs,
               const std::pair<std::string, std::string>& rhs) { return lhs.first < rhs.first; });
  std::string out = "synthetic-cluster/v1";
  for (const std::pair<std::string, std::string>& field : fields) {
    out.push_back('\n');
    out += field.first;
    out.push_back('=');
    out += field.second;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

[[nodiscard]] SyntheticCluster generate_synthetic_cluster(const SyntheticConfig& config) {
  Scenario scenario(config);
  SyntheticCluster& out = scenario.cluster;
  out.config = config;
  out.cluster = config.cluster;

  const std::size_t rack_count = (std::min)(config.rack_count, kMaxRacksPerCluster);

  std::vector<std::size_t> all_indices(rack_count);
  for (std::size_t i = 0; i < rack_count; ++i) {
    all_indices[i] = i;
  }

  // Mandatory racks: clamped to the generated range, deduplicated, sorted.
  std::vector<std::size_t> mandatory;
  mandatory.reserve((std::min)(config.mandatory_rack_indices.size(), rack_count));
  for (std::size_t index : config.mandatory_rack_indices) {
    if (index < rack_count) {
      mandatory.push_back(index);
    }
  }
  std::sort(mandatory.begin(), mandatory.end());
  mandatory.erase(std::unique(mandatory.begin(), mandatory.end()), mandatory.end());

  // Racks registered but never admitted.
  std::vector<std::size_t> missing_candidates;
  missing_candidates.reserve(rack_count);
  for (std::size_t index : all_indices) {
    if (!std::binary_search(mandatory.begin(), mandatory.end(), index)) {
      missing_candidates.push_back(index);
    }
  }
  const std::size_t missing_target =
      (std::min)(config.missing_rack_count, missing_candidates.size());
  std::vector<std::size_t> missing =
      select_subset(std::move(missing_candidates), missing_target, scenario.rng);
  std::sort(missing.begin(), missing.end());

  out.racks.reserve(rack_count);
  std::vector<std::size_t> included;
  included.reserve(rack_count - missing.size());
  for (std::size_t index : all_indices) {
    out.racks.push_back(synthetic_rack_id(index));
    if (!std::binary_search(missing.begin(), missing.end(), index)) {
      included.push_back(index);
    }
  }
  out.missing_racks = rack_ids_of(missing);

  // Racks whose earlier reference is superseded, and racks whose generation is
  // simply advanced. The two sets are disjoint and deterministic.
  const std::size_t stale_target = (std::min)(config.stale_rack_count, included.size());
  std::vector<std::size_t> stale = select_subset(included, stale_target, scenario.rng);
  std::sort(stale.begin(), stale.end());
  std::vector<std::size_t> advanced_candidates;
  advanced_candidates.reserve(included.size());
  for (std::size_t index : included) {
    if (!std::binary_search(stale.begin(), stale.end(), index)) {
      advanced_candidates.push_back(index);
    }
  }
  const std::size_t advanced_target =
      (std::min)(config.rack_generation_changes, advanced_candidates.size());
  std::vector<std::size_t> advanced =
      select_subset(std::move(advanced_candidates), advanced_target, scenario.rng);
  std::sort(advanced.begin(), advanced.end());
  out.stale_racks = rack_ids_of(stale);

  // Racks deliberately left without any inter-rack link.
  std::vector<std::size_t> partition_candidates;
  partition_candidates.reserve(included.size());
  for (std::size_t index : included) {
    if (!std::binary_search(mandatory.begin(), mandatory.end(), index)) {
      partition_candidates.push_back(index);
    }
  }
  const std::size_t partition_target =
      (std::min)(config.partition_count, partition_candidates.size());
  std::vector<std::size_t> partitioned =
      select_subset(std::move(partition_candidates), partition_target, scenario.rng);
  std::sort(partitioned.begin(), partitioned.end());

  // Compositions, drawn once so references and capacity quantities agree.
  scenario.compositions.reserve(rack_count);
  for (std::size_t index = 0; index < rack_count; ++index) {
    scenario.compositions.push_back(
        make_composition(index, config.heterogeneous, scenario.rng));
  }
  scenario.rack_publications.assign(rack_count, 0);

  // -- 1. declare -----------------------------------------------------------
  {
    MutationRequest request = scenario.base_request(MutationKind::DeclareCluster);
    request.declared_lifecycle = ClusterLifecycle::Forming;
    request.readiness_contract.minimum_current_racks = included.size();
    request.readiness_contract.mandatory_racks =
        mandatory.empty() ? rack_ids_of(included) : rack_ids_of(mandatory);
    sort_unique_racks(request.readiness_contract.mandatory_racks);
    request.readiness_contract.require_all_active_racks_current = false;
    request.readiness_contract.require_connectivity_evidence = false;
    request.readiness_contract.minimum_current_links = 0;
    request.readiness_contract.allow_partial = true;
    request.readiness_contract.allow_degraded = true;
    request.readiness_contract.require_no_conflicts = true;
    request.evidence = synthetic_stamp(0);
    request.requested_at = request.evidence.observed_at;
    scenario.push(std::move(request));
  }

  // -- 2. register every rack publisher ------------------------------------
  for (std::size_t index : all_indices) {
    MutationRequest request = scenario.base_request(MutationKind::RegisterRackPublisher);
    scenario.rack_publications[index] = 1;
    request.authority.rack = synthetic_rack_id(index);
    request.authority.rack_generation = RackGeneration::from_raw(1);
    request.authority.publication = PublicationGeneration::from_raw(1);
    if (config.boot.known()) {
      request.authority.boot = config.boot;
    }
    if (config.publisher.known()) {
      request.authority.publisher = config.publisher;
    }
    request.rack_reference = scenario.make_reference(index, RackGeneration::from_raw(1), 1);
    request.evidence = synthetic_stamp(static_cast<std::int64_t>(index));
    request.requested_at = request.evidence.observed_at;
    scenario.push(std::move(request));
  }

  // -- 3. admit the included racks -----------------------------------------
  for (std::size_t index : included) {
    MutationRequest request = scenario.base_request(MutationKind::AddRack);
    scenario.rack_publications[index] = 2;
    request.authority.rack = synthetic_rack_id(index);
    request.authority.rack_generation = RackGeneration::from_raw(2);
    request.authority.publication = PublicationGeneration::from_raw(2);
    if (config.boot.known()) {
      request.authority.boot = config.boot;
    }
    if (config.publisher.known()) {
      request.authority.publisher = config.publisher;
    }
    request.membership = RackMembershipState::Active;
    request.rack_reference = scenario.make_reference(index, RackGeneration::from_raw(2), 2);
    request.evidence = synthetic_stamp(static_cast<std::int64_t>(index));
    request.requested_at = request.evidence.observed_at;
    scenario.push(std::move(request));
  }

  const std::size_t placement_group = effective_group_size(
      config.racks_per_placement_domain, included.size(), kMaxDomainsPerClass);
  const std::size_t placement_count = group_count(included.size(), placement_group);
  const std::size_t degraded_count =
      (std::min)(config.degraded_domain_count, placement_count);

  // -- 4. placement domains -------------------------------------------------
  static const PlacementDomainClass kPlacementClasses[] = {
      PlacementDomainClass::LowLatencyFabric, PlacementDomainClass::AvailabilityDomain,
      PlacementDomainClass::NetworkTier,      PlacementDomainClass::PowerBoundary,
      PlacementDomainClass::StorageLocality,  PlacementDomainClass::FailureDomain,
      PlacementDomainClass::PolicyClass};
  out.placement_domains.reserve(placement_count);
  for (std::size_t domain_index = 0; domain_index < placement_count; ++domain_index) {
    const std::size_t begin = domain_index * placement_group;
    const std::size_t end = (std::min)(begin + placement_group, included.size());
    PlacementDomain domain;
    domain.id = synthetic_placement_domain_id(domain_index);
    domain.klass = kPlacementClasses[domain_index % 7];
    domain.racks = member_slice(included, begin, end);
    sort_unique_racks(domain.racks);
    if (domain_index < degraded_count && domain.racks.size() > 1) {
      domain.racks.pop_back();
    }
    const std::int64_t ordinal = scenario.next_stamp_ordinal();
    domain.header = scenario.make_header(1, "synthetic placement domain " +
                                                std::to_string(domain_index), ordinal);
    MutationRequest request = scenario.base_request(MutationKind::PublishPlacementDomain);
    request.evidence = domain.header.evidence;
    request.requested_at = request.evidence.observed_at;
    request.placement_domain = domain;
    scenario.push(std::move(request));
    out.placement_domains.push_back(std::move(domain));
  }

  // -- 5. capacity domains --------------------------------------------------
  static const CapacityDomainClass kCapacityClasses[] = {
      CapacityDomainClass::RackGroup,      CapacityDomainClass::AcceleratorPool,
      CapacityDomainClass::CpuPool,        CapacityDomainClass::NetworkDomain,
      CapacityDomainClass::StorageDomain};
  out.capacity_domains.reserve(placement_count);
  for (std::size_t domain_index = 0; domain_index < placement_count; ++domain_index) {
    const std::size_t begin = domain_index * placement_group;
    const std::size_t end = (std::min)(begin + placement_group, included.size());
    CapacityDomain domain;
    domain.id = synthetic_capacity_domain_id(domain_index);
    domain.klass = kCapacityClasses[domain_index % 5];
    domain.racks = member_slice(included, begin, end);
    sort_unique_racks(domain.racks);
    std::uint64_t devices = 0;
    std::uint64_t host_memory = 0;
    std::uint64_t accelerator_memory = 0;
    for (std::size_t i = begin; i < end && i < included.size(); ++i) {
      const RackCompositionSummary& composition = scenario.compositions[included[i]];
      for (const AcceleratorClassSummary& accelerator : composition.accelerators) {
        devices += accelerator.device_count.value_or(0u);
        accelerator_memory += accelerator.device_memory_bytes.value_or(0ull);
      }
      host_memory += composition.host_memory_bytes.value_or(0ull);
    }
    const auto quantity = [](const char* unit, double value) {
      CapacityQuantity entry;
      entry.unit = unit;
      entry.value = value;
      entry.provenance = EvidenceProvenance::Synthetic;
      entry.aggregated = true;
      return entry;
    };
    domain.quantities.push_back(quantity("accelerator_memory_bytes",
                                         static_cast<double>(accelerator_memory)));
    domain.quantities.push_back(quantity("devices", static_cast<double>(devices)));
    domain.quantities.push_back(quantity("host_memory_bytes", static_cast<double>(host_memory)));
    const std::int64_t ordinal = scenario.next_stamp_ordinal();
    domain.header = scenario.make_header(1, "synthetic capacity domain " +
                                                std::to_string(domain_index), ordinal);
    MutationRequest request = scenario.base_request(MutationKind::PublishCapacityDomain);
    request.evidence = domain.header.evidence;
    request.requested_at = request.evidence.observed_at;
    request.capacity_domain = domain;
    scenario.push(std::move(request));
    out.capacity_domains.push_back(std::move(domain));
  }

  // -- 6. failure domains ---------------------------------------------------
  static const FailureDomainClass kFailureClasses[] = {
      FailureDomainClass::Row,          FailureDomainClass::SwitchPlane,
      FailureDomainClass::PowerFeed,    FailureDomainClass::NetworkPlane,
      FailureDomainClass::AvailabilityZone, FailureDomainClass::CoolingLoop,
      FailureDomainClass::ControlPlane};
  std::size_t failure_group = config.racks_per_failure_domain == 0 ? 1
                                                                  : config.racks_per_failure_domain;
  std::size_t failure_count = group_count(included.size(), failure_group);
  std::size_t availability_count = group_count(failure_count, 2);
  for (std::size_t guard = 0;
       guard < 64 && !included.empty() &&
       failure_count + availability_count > kMaxDomainsPerClass;
       ++guard) {
    failure_group *= 2;
    failure_count = group_count(included.size(), failure_group);
    availability_count = group_count(failure_count, 2);
  }
  out.failure_domains.reserve(failure_count + availability_count);
  for (std::size_t domain_index = 0; domain_index < failure_count; ++domain_index) {
    const std::size_t begin = domain_index * failure_group;
    const std::size_t end = (std::min)(begin + failure_group, included.size());
    FailureDomain domain;
    domain.klass = kFailureClasses[domain_index % 7];
    domain.id = synthetic_failure_domain_id(domain.klass, domain_index);
    domain.racks = member_slice(included, begin, end);
    sort_unique_racks(domain.racks);
    const std::int64_t ordinal = scenario.next_stamp_ordinal();
    domain.header = scenario.make_header(1, "synthetic failure domain " +
                                                std::to_string(domain_index), ordinal);
    MutationRequest request = scenario.base_request(MutationKind::PublishFailureDomain);
    request.evidence = domain.header.evidence;
    request.requested_at = request.evidence.observed_at;
    request.failure_domain = domain;
    scenario.push(std::move(request));
    out.failure_domains.push_back(std::move(domain));
  }
  for (std::size_t zone_index = 0; zone_index < availability_count; ++zone_index) {
    FailureDomain domain;
    domain.klass = FailureDomainClass::AvailabilityZone;
    domain.id = synthetic_failure_domain_id(domain.klass, failure_count + zone_index);
    const std::size_t begin = zone_index * 2 * failure_group;
    const std::size_t end = (std::min)(begin + 2 * failure_group, included.size());
    domain.racks = member_slice(included, begin, end);
    sort_unique_racks(domain.racks);
    const std::int64_t ordinal = scenario.next_stamp_ordinal();
    domain.header = scenario.make_header(
        1, "synthetic availability zone " + std::to_string(zone_index), ordinal);
    MutationRequest request = scenario.base_request(MutationKind::PublishFailureDomain);
    request.evidence = domain.header.evidence;
    request.requested_at = request.evidence.observed_at;
    request.failure_domain = domain;
    scenario.push(std::move(request));
    out.failure_domains.push_back(std::move(domain));
  }

  // Per-rack failure-domain index, so link metadata is computed without
  // rescanning every domain for every link.
  std::vector<std::vector<FailureDomainId>> domains_of_rack(rack_count);
  for (std::size_t domain_index = 0; domain_index < failure_count; ++domain_index) {
    const std::size_t begin = domain_index * failure_group;
    const std::size_t end = (std::min)(begin + failure_group, included.size());
    for (std::size_t i = begin; i < end; ++i) {
      domains_of_rack[included[i]].push_back(out.failure_domains[domain_index].id);
    }
  }
  for (std::size_t zone_index = 0; zone_index < availability_count; ++zone_index) {
    const std::size_t begin = zone_index * 2 * failure_group;
    const std::size_t end = (std::min)(begin + 2 * failure_group, included.size());
    for (std::size_t i = begin; i < end; ++i) {
      domains_of_rack[included[i]].push_back(
          out.failure_domains[failure_count + zone_index].id);
    }
  }
  for (std::vector<FailureDomainId>& ids : domains_of_rack) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  }

  // -- 7. network domains ---------------------------------------------------
  static const ConnectivityClass kConnectivityClasses[] = {
      ConnectivityClass::DirectFabric, ConnectivityClass::SwitchedFabric,
      ConnectivityClass::RoutedPath,   ConnectivityClass::Overlay,
      ConnectivityClass::ManagementOnly};
  const std::size_t network_count =
      (std::min)(config.network_domain_count, kMaxDomainsPerClass);
  std::vector<std::vector<RackId>> network_members(network_count);
  for (std::size_t position = 0; position < included.size(); ++position) {
    if (network_count == 0) {
      break;
    }
    network_members[position % network_count].push_back(synthetic_rack_id(included[position]));
  }
  for (std::size_t domain_index = 0; domain_index < network_count; ++domain_index) {
    NetworkDomain domain;
    domain.id = synthetic_network_domain_id(domain_index);
    domain.connectivity = kConnectivityClasses[domain_index % 5];
    domain.racks = network_members[domain_index];
    sort_unique_racks(domain.racks);
    const std::int64_t ordinal = scenario.next_stamp_ordinal();
    domain.header = scenario.make_header(1, "synthetic network domain " +
                                                std::to_string(domain_index), ordinal);
    MutationRequest request = scenario.base_request(MutationKind::PublishNetworkDomain);
    request.evidence = domain.header.evidence;
    request.requested_at = request.evidence.observed_at;
    request.network_domain = domain;
    scenario.push(std::move(request));
  }

  // -- 8. optional power and cooling hierarchies ---------------------------
  if (config.include_power_cooling && placement_count > 0) {
    const std::size_t hierarchy_count = (std::min)(placement_count, kMaxDomainsPerClass);
    for (std::size_t domain_index = 0; domain_index < hierarchy_count; ++domain_index) {
      PowerDomain domain;
      domain.id = make_id<PowerDomainId>("power-syn-" + std::to_string(domain_index));
      const std::size_t begin = domain_index * placement_group;
      const std::size_t end = (std::min)(begin + placement_group, included.size());
      domain.racks = member_slice(included, begin, end);
      sort_unique_racks(domain.racks);
      if (domain_index != 0) {
        domain.parent = make_id<PowerDomainId>("power-syn-0");
      }
      const std::int64_t ordinal = scenario.next_stamp_ordinal();
      domain.header = scenario.make_header(1, "synthetic power domain " +
                                                  std::to_string(domain_index), ordinal);
      MutationRequest request = scenario.base_request(MutationKind::PublishPowerDomain);
      request.evidence = domain.header.evidence;
      request.requested_at = request.evidence.observed_at;
      request.power_domain = domain;
      scenario.push(std::move(request));
    }
    for (std::size_t domain_index = 0; domain_index < hierarchy_count; ++domain_index) {
      CoolingDomain domain;
      domain.id = make_id<CoolingDomainId>("cooling-syn-" + std::to_string(domain_index));
      const std::size_t begin = domain_index * placement_group;
      const std::size_t end = (std::min)(begin + placement_group, included.size());
      domain.racks = member_slice(included, begin, end);
      sort_unique_racks(domain.racks);
      if (domain_index != 0) {
        domain.parent = make_id<CoolingDomainId>("cooling-syn-0");
      }
      const std::int64_t ordinal = scenario.next_stamp_ordinal();
      domain.header = scenario.make_header(1, "synthetic cooling domain " +
                                                  std::to_string(domain_index), ordinal);
      MutationRequest request = scenario.base_request(MutationKind::PublishCoolingDomain);
      request.evidence = domain.header.evidence;
      request.requested_at = request.evidence.observed_at;
      request.cooling_domain = domain;
      scenario.push(std::move(request));
    }
  }

  // -- 9. topology supersession (before links carry an epoch) --------------
  const std::size_t epoch_changes = (std::min)(config.topology_epoch_changes,
                                               kMaxTopologyEpochChanges);
  for (std::size_t step = 0; step < epoch_changes; ++step) {
    MutationRequest request = scenario.base_request(MutationKind::SupersedeTopology);
    const auto target = scenario.topology_epoch.next();
    if (!target.has_value()) {
      break;
    }
    request.target_topology_epoch = *target;
    request.topology_reason = kTopologyReason;
    request.evidence = synthetic_stamp(scenario.next_stamp_ordinal());
    request.requested_at = request.evidence.observed_at;
    scenario.push(std::move(request));
    scenario.topology_epoch = *target;
    if (const auto next_generation = scenario.topology_generation.next();
        next_generation.has_value()) {
      scenario.topology_generation = *next_generation;
    }
  }

  // -- 10. inter-rack links -------------------------------------------------
  std::vector<std::size_t> connectable;
  connectable.reserve(included.size());
  for (std::size_t index : included) {
    if (!std::binary_search(partitioned.begin(), partitioned.end(), index)) {
      connectable.push_back(index);
    }
  }
  const std::uint64_t connectable_count = static_cast<std::uint64_t>(connectable.size());
  const std::uint64_t total_pairs =
      connectable_count > 1 ? connectable_count * (connectable_count - 1) / 2 : 0;
  double density = config.link_density;
  if (!(density >= 0.0)) {
    density = 0.0;
  }
  if (density > 1.0) {
    density = 1.0;
  }
  std::uint64_t link_target =
      static_cast<std::uint64_t>(density * static_cast<double>(total_pairs) + 0.5);
  if (link_target > total_pairs) {
    link_target = total_pairs;
  }
  if (link_target > kMaxInterRackLinks) {
    link_target = kMaxInterRackLinks;
  }

  std::vector<std::pair<std::size_t, std::size_t>> selected_pairs;
  selected_pairs.reserve(static_cast<std::size_t>(link_target));
  std::uint64_t remaining = link_target;
  std::uint64_t ordinal = 0;
  for (std::size_t i = 0; i < connectable.size() && remaining > 0; ++i) {
    for (std::size_t j = i + 1; j < connectable.size() && remaining > 0; ++j) {
      const std::uint64_t left = total_pairs - ordinal;
      if (left != 0 && scenario.rng.below(left) < remaining) {
        selected_pairs.emplace_back(i, j);
        --remaining;
      }
      ++ordinal;
    }
  }

  out.links.reserve(selected_pairs.size());
  std::uint64_t link_publication = 0;
  for (const std::pair<std::size_t, std::size_t>& pair : selected_pairs) {
    const std::size_t source_index = connectable[pair.first];
    const std::size_t destination_index = connectable[pair.second];
    InterRackLink link;
    link.source = synthetic_rack_id(source_index);
    link.destination = synthetic_rack_id(destination_index);
    link.id = synthetic_link_id(link.source, link.destination);
    const bool unidirectional =
        config.asymmetric_connectivity && ((scenario.rng.next() & 1ull) != 0ull);
    link.direction = unidirectional ? LinkDirection::Unidirectional : LinkDirection::Bidirectional;
    link.connectivity = network_count == 0
                            ? ConnectivityClass::DirectFabric
                            : kConnectivityClasses[(source_index % network_count) % 5];
    if (network_count != 0) {
      link.network_domain = synthetic_network_domain_id(source_index % network_count);
    }
    link.source_endpoint = endpoint_id(source_index);
    link.destination_endpoint = endpoint_id(destination_index);
    link.bandwidth_class = static_cast<BandwidthClass>(1 + (source_index % 4));
    link.nominal_bandwidth_bps = 100000000000ull * (1ull + static_cast<std::uint64_t>(source_index % 4));
    link.latency_class = static_cast<LatencyClass>(1 + ((source_index + destination_index) % 4));
    link.nominal_latency_nanos = 400ull + static_cast<std::uint64_t>(destination_index % 64) * 10ull;
    link.hop_count = 1u;
    link.reachability = Reachability::Reachable;
    link.health = HealthState::Healthy;
    std::set_intersection(domains_of_rack[source_index].begin(),
                          domains_of_rack[source_index].end(),
                          domains_of_rack[destination_index].begin(),
                          domains_of_rack[destination_index].end(),
                          std::back_inserter(link.failure_domains));
    if (link.failure_domains.size() > kMaxFailureDomainRefs) {
      link.failure_domains.resize(kMaxFailureDomainRefs);
    }
    ++link_publication;
    link.header = scenario.make_header(1, "synthetic inter-rack link " + link.source.value() +
                                              " to " + link.destination.value(),
                                       scenario.next_stamp_ordinal());
    link.topology_epoch = scenario.topology_epoch;
    link.topology_generation = scenario.topology_generation;
    link.publication = PublicationGeneration::from_raw(link_publication);
    if (config.boot.known()) {
      link.boot = config.boot;
    }
    MutationRequest request = scenario.base_request(MutationKind::PublishInterRackLink);
    request.evidence = link.header.evidence;
    request.requested_at = request.evidence.observed_at;
    request.link = link;
    scenario.push(std::move(request));
    out.links.push_back(std::move(link));
  }

  // -- 11. cluster constraint ----------------------------------------------
  if (!included.empty() && kMaxConstraints > 0) {
    ClusterConstraint constraint;
    constraint.id = make_id<ConstraintId>("constraint-syn-0");
    constraint.kind = ConstraintKind::PlacementScope;
    constraint.racks = rack_ids_of(included);
    sort_unique_racks(constraint.racks);
    if (constraint.racks.size() > kMaxDomainMembers) {
      constraint.racks.resize(kMaxDomainMembers);
    }
    if (!out.placement_domains.empty()) {
      constraint.domain_ref = "placement_domain/" + out.placement_domains.front().id.value();
    }
    constraint.statement = "synthetic scenario keeps every admitted rack inside one declared "
                           "placement scope";
    constraint.header = scenario.make_header(1, "synthetic placement constraint",
                                             scenario.next_stamp_ordinal());
    MutationRequest request = scenario.base_request(MutationKind::PublishConstraint);
    request.evidence = constraint.header.evidence;
    request.requested_at = request.evidence.observed_at;
    request.constraint = constraint;
    scenario.push(std::move(request));
  }

  // -- 12. rack generation changes -----------------------------------------
  for (std::size_t index : stale) {
    MutationRequest request = scenario.base_request(MutationKind::UpdateRackGeneration);
    const std::uint64_t next_publication = scenario.rack_publications[index] + 1;
    scenario.rack_publications[index] = next_publication;
    request.authority.rack = synthetic_rack_id(index);
    request.authority.rack_generation = RackGeneration::from_raw(2);
    request.authority.publication = PublicationGeneration::from_raw(next_publication);
    if (config.boot.known()) {
      request.authority.boot = config.boot;
    }
    if (config.publisher.known()) {
      request.authority.publisher = config.publisher;
    }
    request.expected_rack_generation = RackGeneration::from_raw(2);
    request.new_rack_generation = RackGeneration::from_raw(3);
    request.rack_reference = scenario.make_reference(index, RackGeneration::from_raw(3),
                                                     next_publication);
    request.rack_reference.composition.composition_label = "synthetic-superseding-composition";
    request.evidence = synthetic_stamp(static_cast<std::int64_t>(index));
    request.requested_at = request.evidence.observed_at;
    scenario.push(std::move(request));
  }
  for (std::size_t index : advanced) {
    MutationRequest request = scenario.base_request(MutationKind::UpdateRackGeneration);
    const std::uint64_t next_publication = scenario.rack_publications[index] + 1;
    scenario.rack_publications[index] = next_publication;
    request.authority.rack = synthetic_rack_id(index);
    request.authority.rack_generation = RackGeneration::from_raw(2);
    request.authority.publication = PublicationGeneration::from_raw(next_publication);
    if (config.boot.known()) {
      request.authority.boot = config.boot;
    }
    if (config.publisher.known()) {
      request.authority.publisher = config.publisher;
    }
    request.expected_rack_generation = RackGeneration::from_raw(2);
    request.new_rack_generation = RackGeneration::from_raw(3);
    request.rack_reference = scenario.make_reference(index, RackGeneration::from_raw(3),
                                                     next_publication);
    request.evidence = synthetic_stamp(static_cast<std::int64_t>(index));
    request.requested_at = request.evidence.observed_at;
    scenario.push(std::move(request));
  }

  return out;
}

}  // namespace cluster_fabric
