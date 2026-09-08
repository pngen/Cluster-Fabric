// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Randomized property coverage for the deterministic synthetic cluster
// laboratory.
//
// Every scenario is derived from an explicit seed. No std::random_device and no
// time-based seeding appears anywhere in this file, so a failing seed is always
// reproducible from the parameters printed in the failure message. Each seed
// exercises every SyntheticConfig knob, applies the complete generated request
// sequence to a real ClusterCoordinator, and checks the canonical state that
// results.

#include "harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

namespace {

constexpr std::size_t kSeedCount = 200;
constexpr std::uint64_t kSeedBase = 0x5EED0000ull;

[[nodiscard]] std::string seed_text(const SyntheticConfig& config) {
  return "seed=" + std::to_string(config.seed);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/// A base configuration with every identity present. Callers override counts.
[[nodiscard]] SyntheticConfig base_config(std::uint64_t seed, std::size_t index) {
  SyntheticConfig config;
  config.seed = seed;
  config.cluster = *ClusterId::parse("synth-property-cluster");
  config.publisher = *RackPublisherId::parse("synth-property-publisher");
  config.boot = *RackAgentBootId::parse("synth-property-boot-" + std::to_string(index));
  return config;
}

/// Exercises every knob of SyntheticConfig. The values are derived from the
/// seed index only, so the configuration itself is reproducible.
[[nodiscard]] SyntheticConfig make_property_config(std::size_t index) {
  SyntheticConfig config = base_config(kSeedBase + static_cast<std::uint64_t>(index), index);
  config.rack_count = 3 + (index % 11);
  config.racks_per_placement_domain = 1 + (index % 3);
  config.racks_per_failure_domain = 1 + (index % 2);
  config.network_domain_count = index % 4;
  config.link_density = static_cast<double>(index % 11) / 10.0;
  config.heterogeneous = (index % 2) == 0;
  config.missing_rack_count = index % 3;
  config.stale_rack_count = index % 3;
  config.rack_generation_changes = (index / 2) % 3;
  config.degraded_domain_count = index % 2;
  config.partition_count = index % 2;
  config.asymmetric_connectivity = (index % 3) == 0;
  config.topology_epoch_changes = index % 3;
  config.include_power_cooling = (index % 2) == 1;
  config.mandatory_rack_indices = {std::size_t{0}, index % 4};
  return config;
}

// ---------------------------------------------------------------------------
// Assertion helpers
// ---------------------------------------------------------------------------

void expect_valid_identity(std::string_view value, const char* what,
                           const SyntheticConfig& config) {
  if (value.empty()) {
    return;  // The explicit UNKNOWN identity is never a generated value.
  }
  const IdentityValidation validation = validate_identity(value);
  if (!validation.ok()) {
    CF_FAIL(std::string(what) + " is not a valid identity: '" + std::string(value) + "' status=" +
            std::to_string(static_cast<int>(validation.status)) + " position=" +
            std::to_string(validation.position) + " " + seed_text(config));
  }
}

void expect_synthetic_stamp(const EvidenceStamp& stamp, const char* what,
                            const SyntheticConfig& config) {
  if (stamp == EvidenceStamp{}) {
    return;  // Absent evidence is absent, not misclassified.
  }
  if (stamp.provenance != EvidenceProvenance::Synthetic) {
    CF_FAIL(std::string(what) + " carries provenance " +
            std::string(to_string(stamp.provenance)) + " instead of SYNTHETIC " +
            seed_text(config));
  }
}

void expect_domain_header(const DomainHeader& header, const char* what,
                          const SyntheticConfig& config) {
  expect_synthetic_stamp(header.evidence, what, config);
  // A domain label is free-form bounded text, not an identity.
  if (!header.label.empty() && !validate_label(header.label).ok()) {
    CF_FAIL(std::string(what) + " label is not a valid bounded label: '" + header.label + "' " +
            seed_text(config));
  }
}

void expect_request_is_synthetic(const MutationRequest& request, const SyntheticConfig& config) {
  const char* kind = to_string(request.kind).data();
  if (request.evidence.provenance != EvidenceProvenance::Synthetic) {
    CF_FAIL(std::string("request ") + kind + " evidence provenance is " +
            std::string(to_string(request.evidence.provenance)) + " instead of SYNTHETIC " +
            seed_text(config));
  }
  if (!request.evidence.observed_at.known()) {
    CF_FAIL(std::string("request ") + kind + " carries an unknown observation time " +
            seed_text(config));
  }
  expect_synthetic_stamp(request.rack_reference.evidence, "rack_reference.evidence", config);
  for (const RackEndpoint& endpoint : request.rack_reference.endpoints) {
    expect_synthetic_stamp(endpoint.evidence, "rack_reference.endpoint.evidence", config);
  }
  for (const RackFailureDomainHint& hint : request.rack_reference.failure_domain_hints) {
    expect_synthetic_stamp(hint.evidence, "rack_reference.hint.evidence", config);
  }
  if (request.rack_reference.composition.provenance != EvidenceProvenance::Unknown &&
      request.rack_reference.composition.provenance != EvidenceProvenance::Synthetic) {
    CF_FAIL(std::string("rack_reference.composition provenance is ") +
            std::string(to_string(request.rack_reference.composition.provenance)) +
            " instead of SYNTHETIC " + seed_text(config));
  }
  if (request.placement_domain.has_value()) {
    expect_domain_header(request.placement_domain->header, "placement_domain.header", config);
  }
  if (request.capacity_domain.has_value()) {
    expect_domain_header(request.capacity_domain->header, "capacity_domain.header", config);
    for (const CapacityQuantity& quantity : request.capacity_domain->quantities) {
      if (quantity.provenance != EvidenceProvenance::Synthetic) {
        CF_FAIL("capacity quantity provenance is " +
                std::string(to_string(quantity.provenance)) + " instead of SYNTHETIC " +
                seed_text(config));
      }
    }
  }
  if (request.failure_domain.has_value()) {
    expect_domain_header(request.failure_domain->header, "failure_domain.header", config);
  }
  if (request.network_domain.has_value()) {
    expect_domain_header(request.network_domain->header, "network_domain.header", config);
  }
  if (request.power_domain.has_value()) {
    expect_domain_header(request.power_domain->header, "power_domain.header", config);
  }
  if (request.cooling_domain.has_value()) {
    expect_domain_header(request.cooling_domain->header, "cooling_domain.header", config);
  }
  if (request.link.has_value()) {
    expect_domain_header(request.link->header, "link.header", config);
  }
  if (request.constraint.has_value()) {
    expect_domain_header(request.constraint->header, "constraint.header", config);
  }
}

void expect_request_identities_valid(const MutationRequest& request,
                                     const SyntheticConfig& config) {
  expect_valid_identity(request.cluster.view(), "request.cluster", config);
  if (request.authority.rack.has_value()) {
    expect_valid_identity(request.authority.rack->view(), "authority.rack", config);
  }
  if (request.authority.publisher.has_value()) {
    expect_valid_identity(request.authority.publisher->view(), "authority.publisher", config);
  }
  if (request.authority.boot.has_value()) {
    expect_valid_identity(request.authority.boot->view(), "authority.boot", config);
  }
  expect_valid_identity(request.rack_reference.rack.view(), "rack_reference.rack", config);
  for (const RackEndpoint& endpoint : request.rack_reference.endpoints) {
    expect_valid_identity(endpoint.id.view(), "rack_reference.endpoint.id", config);
  }
  for (const RackFailureDomainHint& hint : request.rack_reference.failure_domain_hints) {
    expect_valid_identity(hint.id.view(), "rack_reference.hint.id", config);
  }
  if (request.placement_domain.has_value()) {
    expect_valid_identity(request.placement_domain->id.view(), "placement_domain.id", config);
    for (const RackId& rack : request.placement_domain->racks) {
      expect_valid_identity(rack.view(), "placement_domain.rack", config);
    }
  }
  if (request.capacity_domain.has_value()) {
    expect_valid_identity(request.capacity_domain->id.view(), "capacity_domain.id", config);
    for (const RackId& rack : request.capacity_domain->racks) {
      expect_valid_identity(rack.view(), "capacity_domain.rack", config);
    }
  }
  if (request.failure_domain.has_value()) {
    expect_valid_identity(request.failure_domain->id.view(), "failure_domain.id", config);
    for (const RackId& rack : request.failure_domain->racks) {
      expect_valid_identity(rack.view(), "failure_domain.rack", config);
    }
  }
  if (request.network_domain.has_value()) {
    expect_valid_identity(request.network_domain->id.view(), "network_domain.id", config);
    for (const RackId& rack : request.network_domain->racks) {
      expect_valid_identity(rack.view(), "network_domain.rack", config);
    }
  }
  if (request.power_domain.has_value()) {
    expect_valid_identity(request.power_domain->id.view(), "power_domain.id", config);
    if (request.power_domain->parent.has_value()) {
      expect_valid_identity(request.power_domain->parent->view(), "power_domain.parent", config);
    }
    for (const RackId& rack : request.power_domain->racks) {
      expect_valid_identity(rack.view(), "power_domain.rack", config);
    }
  }
  if (request.cooling_domain.has_value()) {
    expect_valid_identity(request.cooling_domain->id.view(), "cooling_domain.id", config);
    if (request.cooling_domain->parent.has_value()) {
      expect_valid_identity(request.cooling_domain->parent->view(), "cooling_domain.parent", config);
    }
    for (const RackId& rack : request.cooling_domain->racks) {
      expect_valid_identity(rack.view(), "cooling_domain.rack", config);
    }
  }
  if (request.link.has_value()) {
    expect_valid_identity(request.link->id.view(), "link.id", config);
    expect_valid_identity(request.link->source.view(), "link.source", config);
    expect_valid_identity(request.link->destination.view(), "link.destination", config);
    if (request.link->source_endpoint.has_value()) {
      expect_valid_identity(request.link->source_endpoint->view(), "link.source_endpoint", config);
    }
    if (request.link->destination_endpoint.has_value()) {
      expect_valid_identity(request.link->destination_endpoint->view(), "link.destination_endpoint",
                            config);
    }
    if (request.link->network_domain.has_value()) {
      expect_valid_identity(request.link->network_domain->view(), "link.network_domain", config);
    }
    if (request.link->link_domain.has_value()) {
      expect_valid_identity(request.link->link_domain->view(), "link.link_domain", config);
    }
    for (const FailureDomainId& domain : request.link->failure_domains) {
      expect_valid_identity(domain.view(), "link.failure_domain", config);
    }
  }
  if (request.constraint.has_value()) {
    expect_valid_identity(request.constraint->id.view(), "constraint.id", config);
    for (const RackId& rack : request.constraint->racks) {
      expect_valid_identity(rack.view(), "constraint.rack", config);
    }
  }
}

/// Encodes every request into its canonical wire form.
[[nodiscard]] std::vector<std::string> encode_requests(const SyntheticCluster& scenario) {
  std::vector<std::string> encoded;
  encoded.reserve(scenario.requests.size());
  for (std::size_t index = 0; index < scenario.requests.size(); ++index) {
    ByteWriter writer;
    const bool ok = encode_mutation_request(scenario.requests[index], writer);
    if (!ok || !writer.ok()) {
      CF_FAIL("encode_mutation_request failed for request " + std::to_string(index) + " kind " +
              std::string(to_string(scenario.requests[index].kind)) + " status " +
              std::string(to_string(writer.status())) + " " + seed_text(scenario.config));
    }
    encoded.push_back(writer.data());
  }
  return encoded;
}

void expect_encodings_identical(const SyntheticCluster& scenario) {
  const SyntheticCluster regenerated = generate_synthetic_cluster(scenario.config);
  const std::vector<std::string> first = encode_requests(scenario);
  const std::vector<std::string> second = encode_requests(regenerated);
  if (first.size() != second.size()) {
    CF_FAIL("regenerating the identical configuration produced " +
            std::to_string(second.size()) + " requests instead of " +
            std::to_string(first.size()) + " " + seed_text(scenario.config));
  }
  for (std::size_t index = 0; index < first.size(); ++index) {
    if (first[index] != second[index]) {
      CF_FAIL("regenerating the identical configuration changed request " +
              std::to_string(index) + " kind " +
              std::string(to_string(scenario.requests[index].kind)) + " (encoded " +
              std::to_string(first[index].size()) + " vs " + std::to_string(second[index].size()) +
              " bytes) " + seed_text(scenario.config));
    }
  }
}

/// Applies every request. The first rejection is reported with its reason, the
/// seed and the exact reproduction parameters.
void apply_or_fail(const SyntheticCluster& scenario, ClusterCoordinator& coordinator) {
  for (std::size_t index = 0; index < scenario.requests.size(); ++index) {
    const MutationRequest& request = scenario.requests[index];
    const MutationResult result = coordinator.submit(request);
    if (result.accepted()) {
      continue;
    }
    CF_FAIL("request " + std::to_string(index) + " of " +
            std::to_string(scenario.requests.size()) + " was rejected: kind=" +
            std::string(to_string(request.kind)) + " outcome=" +
            std::string(to_string(result.outcome)) + " reason=" +
            std::string(to_string(result.reason)) + " error=" + result.error.describe() +
            "\n" + scenario.reproduction_parameters());
  }
}

void expect_count(std::size_t actual, std::size_t expected, const char* what,
                  const SyntheticConfig& config) {
  if (actual != expected) {
    CF_FAIL(std::string(what) + " count mismatch: actual " + std::to_string(actual) +
            ", expected " + std::to_string(expected) + " " + seed_text(config));
  }
}

/// Every generated count must be present in the canonical composition.
void expect_counts_match(const SyntheticCluster& scenario, const ClusterSnapshot& snapshot) {
  const ClusterState& state = snapshot.state();
  const std::size_t included = scenario.racks.size() - scenario.missing_racks.size();
  const std::size_t network_expected =
      (std::min)(scenario.config.network_domain_count, kMaxDomainsPerClass);
  const std::size_t power_expected =
      scenario.config.include_power_cooling && !scenario.placement_domains.empty()
          ? (std::min)(scenario.placement_domains.size(), kMaxDomainsPerClass)
          : 0u;
  const std::size_t constraint_expected = included == 0 ? 0u : 1u;

  expect_count(state.racks.size(), scenario.racks.size(), "rack", scenario.config);
  expect_count(snapshot.rack_bindings().size(), scenario.racks.size(), "rack binding",
               scenario.config);
  expect_count(state.links.size(), scenario.links.size(), "link", scenario.config);
  expect_count(state.placement_domains.size(), scenario.placement_domains.size(), "placement domain",
               scenario.config);
  expect_count(state.capacity_domains.size(), scenario.capacity_domains.size(), "capacity domain",
               scenario.config);
  expect_count(state.failure_domains.size(), scenario.failure_domains.size(), "failure domain",
               scenario.config);
  expect_count(state.network_domains.size(), network_expected, "network domain", scenario.config);
  expect_count(state.power_domains.size(), power_expected, "power domain", scenario.config);
  expect_count(state.cooling_domains.size(), power_expected, "cooling domain", scenario.config);
  expect_count(state.constraints.size(), constraint_expected, "constraint", scenario.config);
  expect_count(state.withdrawn_racks.size(), 0u, "withdrawn rack", scenario.config);
  expect_count(state.retired_racks.size(), 0u, "retired rack", scenario.config);
  expect_count(state.fenced_authorities.size(), 0u, "fenced authority", scenario.config);

  for (const RackId& rack : scenario.racks) {
    const auto it = state.racks.find(rack);
    if (it == state.racks.end()) {
      CF_FAIL("generated rack " + rack.value() + " is absent from canonical state " +
              seed_text(scenario.config));
    }
    const bool is_missing = std::binary_search(scenario.missing_racks.begin(),
                                              scenario.missing_racks.end(), rack);
    const RackMembershipState expected =
        is_missing ? RackMembershipState::Unknown : RackMembershipState::Active;
    if (it->second.membership != expected) {
      CF_FAIL("rack " + rack.value() + " membership is " +
              std::string(to_string(it->second.membership)) + " but the scenario expected " +
              std::string(to_string(expected)) + " " + seed_text(scenario.config));
    }
  }
  for (const RackId& rack : scenario.stale_racks) {
    const auto it = state.racks.find(rack);
    if (it == state.racks.end() || it->second.reference.generation != RackGeneration::from_raw(3)) {
      CF_FAIL("stale rack " + rack.value() + " did not reach the superseding generation " +
              seed_text(scenario.config));
    }
  }
}

/// Runs one scenario end to end against a real coordinator.
void run_scenario(const SyntheticCluster& scenario) {
  CoordinatorConfig config;
  config.cluster = scenario.cluster;
  config.persist_on_commit = false;
  config.readiness_contract = ReadinessContract::permissive();

  ClusterCoordinator coordinator(config);
  const CoordinatorStartOutcome started = coordinator.start();
  if (!started.ok) {
    CF_FAIL("coordinator failed to start: status=" + std::string(to_string(started.status)) +
            " error=" + started.error.describe() + " " + seed_text(scenario.config));
  }
  apply_or_fail(scenario, coordinator);

  const InvariantReport invariants = coordinator.invariants();
  if (!invariants.ok()) {
    CF_FAIL("invariants violated after applying the scenario: " + invariants.describe() + "\n" +
            scenario.reproduction_parameters());
  }

  const ClusterSnapshot snapshot = coordinator.snapshot();
  CF_EXPECT(snapshot.valid());
  const SnapshotValidation validation = coordinator.validate(snapshot);
  if (!validation.current) {
    CF_FAIL("snapshot does not validate as current: " + validation.describe() + " lifecycle=" +
            std::string(to_string(coordinator.readiness().lifecycle)) + "\n" +
            scenario.reproduction_parameters());
  }
  expect_counts_match(scenario, snapshot);
  coordinator.stop();
}

/// Collects every rack identity a request refers to.
[[nodiscard]] std::vector<RackId> referenced_racks(const MutationRequest& request) {
  std::vector<RackId> racks;
  if (request.authority.rack.has_value()) {
    racks.push_back(*request.authority.rack);
  }
  if (request.rack_reference.rack.known()) {
    racks.push_back(request.rack_reference.rack);
  }
  if (request.placement_domain.has_value()) {
    racks.insert(racks.end(), request.placement_domain->racks.begin(),
                 request.placement_domain->racks.end());
  }
  if (request.capacity_domain.has_value()) {
    racks.insert(racks.end(), request.capacity_domain->racks.begin(),
                 request.capacity_domain->racks.end());
  }
  if (request.failure_domain.has_value()) {
    racks.insert(racks.end(), request.failure_domain->racks.begin(),
                 request.failure_domain->racks.end());
  }
  if (request.network_domain.has_value()) {
    racks.insert(racks.end(), request.network_domain->racks.begin(),
                 request.network_domain->racks.end());
  }
  if (request.power_domain.has_value()) {
    racks.insert(racks.end(), request.power_domain->racks.begin(),
                 request.power_domain->racks.end());
  }
  if (request.cooling_domain.has_value()) {
    racks.insert(racks.end(), request.cooling_domain->racks.begin(),
                 request.cooling_domain->racks.end());
  }
  if (request.link.has_value()) {
    racks.push_back(request.link->source);
    racks.push_back(request.link->destination);
  }
  if (request.constraint.has_value()) {
    racks.insert(racks.end(), request.constraint->racks.begin(), request.constraint->racks.end());
  }
  racks.insert(racks.end(), request.racks.begin(), request.racks.end());
  return racks;
}

/// Every rack identity a request mentions must belong to the generated rack set.
void expect_references_are_generated(const SyntheticCluster& scenario) {
  std::vector<RackId> allowed = scenario.racks;
  std::sort(allowed.begin(), allowed.end());
  for (const MutationRequest& request : scenario.requests) {
    for (const RackId& rack : referenced_racks(request)) {
      if (!std::binary_search(allowed.begin(), allowed.end(), rack)) {
        CF_FAIL("request " + std::string(to_string(request.kind)) + " references rack " +
                rack.value() + " which the generator never produced " + seed_text(scenario.config));
      }
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Property cases
// ---------------------------------------------------------------------------

CF_TEST(synthetic_property_scenarios_apply_cleanly_for_200_seeds) {
  for (std::size_t index = 0; index < kSeedCount; ++index) {
    const SyntheticConfig config = make_property_config(index);
    const SyntheticCluster scenario = generate_synthetic_cluster(config);
    CF_EXPECT(!scenario.requests.empty());
    CF_EXPECT_EQ(scenario.racks.size(), config.rack_count);
    CF_EXPECT_EQ(scenario.cluster, config.cluster);
    for (const MutationRequest& request : scenario.requests) {
      expect_request_is_synthetic(request, config);
      expect_request_identities_valid(request, config);
    }
    expect_references_are_generated(scenario);
    expect_encodings_identical(scenario);
    run_scenario(scenario);
  }
}

CF_TEST(synthetic_documented_sizes_build_and_apply) {
  const std::size_t sizes[] = {4u, 32u, 256u};
  for (std::size_t size : sizes) {
    SyntheticConfig config = base_config(20260101u + static_cast<std::uint64_t>(size), size);
    config.rack_count = size;
    config.racks_per_placement_domain = 4;
    config.racks_per_failure_domain = 8;
    config.network_domain_count = 2;
    config.link_density = 0.05;
    config.heterogeneous = true;
    config.include_power_cooling = true;
    config.topology_epoch_changes = 1;
    config.stale_rack_count = size > 4 ? 1 : 0;
    config.rack_generation_changes = size > 4 ? 1 : 0;
    config.degraded_domain_count = 1;
    config.mandatory_rack_indices = {std::size_t{0}};

    const SyntheticCluster scenario = generate_synthetic_cluster(config);
    CF_EXPECT_EQ(scenario.racks.size(), size);
    expect_encodings_identical(scenario);
    run_scenario(scenario);
  }
}

CF_TEST(synthetic_impossible_configuration_is_clamped_deterministically) {
  // A rack count above the hard bound is clamped to the bound instead of being
  // allocated unbounded, and the clamped scenario is still reproducible.
  SyntheticConfig huge = base_config(0xB0B0u, 0);
  huge.rack_count = kMaxRacksPerCluster + 512;
  huge.racks_per_placement_domain = kMaxRacksPerCluster;
  huge.racks_per_failure_domain = kMaxRacksPerCluster;
  huge.network_domain_count = 1;
  huge.link_density = 0.0;
  huge.heterogeneous = false;
  huge.include_power_cooling = false;
  huge.mandatory_rack_indices.clear();

  const SyntheticCluster clamped = generate_synthetic_cluster(huge);
  expect_count(clamped.racks.size(), kMaxRacksPerCluster, "clamped rack", huge);
  CF_EXPECT_EQ(clamped.racks.back(), synthetic_rack_id(kMaxRacksPerCluster - 1));
  CF_EXPECT_EQ(clamped.links.size(), std::size_t{0});
  expect_references_are_generated(clamped);
  expect_encodings_identical(clamped);

  // Absurd knob values are clamped rather than honoured.
  SyntheticConfig absurd = base_config(0xB0B1u, 1);
  absurd.rack_count = 3;
  absurd.link_density = 1.0e9;
  absurd.missing_rack_count = 10'000;
  absurd.stale_rack_count = 10'000;
  absurd.rack_generation_changes = 10'000;
  absurd.degraded_domain_count = 10'000;
  absurd.partition_count = 10'000;
  absurd.topology_epoch_changes = 1'000'000;
  const SyntheticCluster saturated = generate_synthetic_cluster(absurd);
  CF_EXPECT_EQ(saturated.racks.size(), std::size_t{3});
  CF_EXPECT(saturated.missing_racks.size() <= saturated.racks.size());
  CF_EXPECT(saturated.stale_racks.size() <= saturated.racks.size());
  CF_EXPECT(saturated.links.size() <= std::size_t{3});
  std::size_t supersessions = 0;
  for (const MutationRequest& request : saturated.requests) {
    if (request.kind == MutationKind::SupersedeTopology) {
      ++supersessions;
    }
  }
  CF_EXPECT(supersessions > 0);
  CF_EXPECT(supersessions < absurd.topology_epoch_changes);
  expect_encodings_identical(saturated);

  // A non-finite density is treated as zero, never as an unbounded count.
  SyntheticConfig not_a_number = base_config(0xB0B2u, 2);
  not_a_number.rack_count = 4;
  not_a_number.link_density = std::nan("");
  const SyntheticCluster nan_density = generate_synthetic_cluster(not_a_number);
  CF_EXPECT_EQ(nan_density.links.size(), std::size_t{0});

  // A mandatory rack index that does not exist is ignored, and the resulting
  // scenario is still applicable to a real coordinator.
  SyntheticConfig impossible = base_config(0xB0B3u, 3);
  impossible.rack_count = 4;
  impossible.mandatory_rack_indices = {std::size_t{0}, std::size_t{3}, std::size_t{4},
                                       std::size_t{999999}};
  const SyntheticCluster tolerated = generate_synthetic_cluster(impossible);
  CF_EXPECT_EQ(tolerated.racks.size(), std::size_t{4});
  for (const RackId& rack : referenced_racks(tolerated.requests.front())) {
    CF_EXPECT_NE(rack, synthetic_rack_id(999999));
  }
  for (const MutationRequest& request : tolerated.requests) {
    for (const RackId& rack : referenced_racks(request)) {
      CF_EXPECT_NE(rack, synthetic_rack_id(999999));
    }
  }
  expect_encodings_identical(tolerated);
  run_scenario(tolerated);
}

int main() { return cf_test::run("test_synthetic_property"); }
