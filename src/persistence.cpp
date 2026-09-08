// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Versioned, integrity-checked durable state container.

#include "cluster_fabric/persistence.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "cluster_fabric/limits.hpp"
#include "cluster_fabric/protocol.hpp"
#include "cluster_fabric/version.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace cluster_fabric {

std::string_view to_string(PersistenceStatus value) noexcept {
  switch (value) {
    case PersistenceStatus::Ok: return "OK";
    case PersistenceStatus::EmptyInput: return "EMPTY_INPUT";
    case PersistenceStatus::MissingFile: return "MISSING_FILE";
    case PersistenceStatus::BadMagic: return "BAD_MAGIC";
    case PersistenceStatus::UnsupportedVersion: return "UNSUPPORTED_VERSION";
    case PersistenceStatus::TruncatedHeader: return "TRUNCATED_HEADER";
    case PersistenceStatus::TruncatedBody: return "TRUNCATED_BODY";
    case PersistenceStatus::ChecksumMismatch: return "CHECKSUM_MISMATCH";
    case PersistenceStatus::TrailingGarbage: return "TRAILING_GARBAGE";
    case PersistenceStatus::InvalidEnum: return "INVALID_ENUM";
    case PersistenceStatus::AbsurdCount: return "ABSURD_COUNT";
    case PersistenceStatus::BoundsExceeded: return "BOUNDS_EXCEEDED";
    case PersistenceStatus::DuplicateRackIdentity: return "DUPLICATE_RACK_IDENTITY";
    case PersistenceStatus::DanglingReference: return "DANGLING_REFERENCE";
    case PersistenceStatus::InvalidEpoch: return "INVALID_EPOCH";
    case PersistenceStatus::InvalidGeneration: return "INVALID_GENERATION";
    case PersistenceStatus::InvalidIdentity: return "INVALID_IDENTITY";
    case PersistenceStatus::InvalidState: return "INVALID_STATE";
    case PersistenceStatus::PayloadTooLarge: return "PAYLOAD_TOO_LARGE";
    case PersistenceStatus::IoError: return "IO_ERROR";
    case PersistenceStatus::AtomicReplaceFailed: return "ATOMIC_REPLACE_FAILED";
    case PersistenceStatus::TempFileUnavailable: return "TEMP_FILE_UNAVAILABLE";
  }
  return "IO_ERROR";
}

namespace {

inline constexpr std::size_t kContainerHeaderBytes = 16;

/// Process identifier used to make temporary file names unique per process.
[[nodiscard]] std::uint64_t process_identifier() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(getpid());
#endif
}

void write_stamp(ByteWriter& writer, const EvidenceStamp& stamp) {
  writer.u8(static_cast<std::uint8_t>(stamp.provenance));
  writer.i64(stamp.observed_at.millis());
  writer.u8(static_cast<std::uint8_t>(stamp.freshness));
  writer.i64(stamp.ttl_millis);
}

[[nodiscard]] bool read_stamp(ByteReader& reader, EvidenceStamp& stamp) {
  const std::optional<std::uint8_t> provenance = reader.u8();
  const std::optional<std::int64_t> observed = reader.i64();
  const std::optional<std::uint8_t> freshness = reader.u8();
  const std::optional<std::int64_t> ttl = reader.i64();
  if (!reader.ok() || !provenance.has_value() || !observed.has_value() ||
      !freshness.has_value() || !ttl.has_value()) {
    return false;
  }
  if (*provenance > static_cast<std::uint8_t>(EvidenceProvenance::Reconstructed)) {
    reader.mark_invalid();
    return false;
  }
  if (*freshness > static_cast<std::uint8_t>(Freshness::RevalidationRequired)) {
    reader.mark_invalid();
    return false;
  }
  stamp.provenance = static_cast<EvidenceProvenance>(*provenance);
  stamp.observed_at = Timestamp::from_unix_millis(*observed);
  stamp.freshness = static_cast<Freshness>(*freshness);
  stamp.ttl_millis = *ttl;
  return true;
}

void write_header(ByteWriter& writer, const DomainHeader& header) {
  write_stamp(writer, header.evidence);
  writer.u64(header.generation.value());
  writer.optional_present(header.publisher.has_value());
  if (header.publisher.has_value()) {
    writer.text(header.publisher->view());
  }
  writer.u64(header.cluster_epoch.value());
  writer.u64(header.coordinator_epoch.value());
  writer.text(header.label);
}

[[nodiscard]] bool read_header(ByteReader& reader, DomainHeader& header) {
  if (!read_stamp(reader, header.evidence)) {
    return false;
  }
  const std::optional<std::uint64_t> generation = reader.u64();
  const std::optional<bool> has_publisher = reader.boolean();
  if (!reader.ok() || !generation.has_value() || !has_publisher.has_value()) {
    return false;
  }
  header.generation = DomainGeneration::from_raw(*generation);
  if (*has_publisher) {
    const std::optional<std::string> publisher = reader.text();
    if (!publisher.has_value()) {
      return false;
    }
    const std::optional<RackPublisherId> parsed = RackPublisherId::parse(*publisher);
    if (!parsed.has_value()) {
      reader.mark_invalid();
      return false;
    }
    header.publisher = *parsed;
  }
  const std::optional<std::uint64_t> cluster_epoch = reader.u64();
  const std::optional<std::uint64_t> coordinator_epoch = reader.u64();
  const std::optional<std::string> label = reader.text();
  if (!reader.ok() || !cluster_epoch.has_value() || !coordinator_epoch.has_value() ||
      !label.has_value()) {
    return false;
  }
  header.cluster_epoch = ClusterEpoch::from_raw(*cluster_epoch);
  header.coordinator_epoch = CoordinatorEpoch::from_raw(*coordinator_epoch);
  header.label = *label;
  return true;
}

void write_rack_ids(ByteWriter& writer, const std::vector<RackId>& racks) {
  writer.u32(static_cast<std::uint32_t>(racks.size()));
  for (const RackId& rack : racks) {
    writer.text(rack.view());
  }
}

[[nodiscard]] bool read_rack_ids(ByteReader& reader, std::vector<RackId>& racks,
                                 std::size_t bound) {
  const std::optional<std::uint32_t> count = reader.u32();
  if (!reader.ok() || !count.has_value()) {
    return false;
  }
  if (*count > bound) {
    reader.mark_invalid();
    return false;
  }
  racks.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    const std::optional<std::string> text = reader.text();
    if (!text.has_value()) {
      return false;
    }
    const std::optional<RackId> parsed = RackId::parse(*text);
    if (!parsed.has_value()) {
      reader.mark_invalid();
      return false;
    }
    racks.push_back(*parsed);
  }
  return true;
}

void write_composition(ByteWriter& writer, const RackCompositionSummary& composition) {
  writer.u8(static_cast<std::uint8_t>(composition.provenance));
  writer.u32(static_cast<std::uint32_t>(composition.accelerators.size()));
  for (const AcceleratorClassSummary& summary : composition.accelerators) {
    writer.u8(static_cast<std::uint8_t>(summary.vendor));
    writer.text(summary.family);
    writer.optional_present(summary.device_count.has_value());
    writer.u32(summary.device_count.value_or(0));
    writer.optional_present(summary.device_memory_bytes.has_value());
    writer.u64(summary.device_memory_bytes.value_or(0));
  }
  writer.optional_present(composition.cpu_sockets.has_value());
  writer.u32(composition.cpu_sockets.value_or(0));
  writer.optional_present(composition.cpu_cores.has_value());
  writer.u32(composition.cpu_cores.value_or(0));
  writer.optional_present(composition.host_memory_bytes.has_value());
  writer.u64(composition.host_memory_bytes.value_or(0));
  writer.optional_present(composition.nic_count.has_value());
  writer.u32(composition.nic_count.value_or(0));
  writer.optional_present(composition.switch_count.has_value());
  writer.u32(composition.switch_count.value_or(0));
  writer.text(composition.composition_label);
}

[[nodiscard]] bool read_composition(ByteReader& reader, RackCompositionSummary& composition) {
  const std::optional<std::uint8_t> provenance = reader.u8();
  const std::optional<std::uint32_t> count = reader.u32();
  if (!reader.ok() || !provenance.has_value() || !count.has_value()) {
    return false;
  }
  if (*provenance > static_cast<std::uint8_t>(EvidenceProvenance::Reconstructed) ||
      *count > kMaxCapacityQuantities) {
    reader.mark_invalid();
    return false;
  }
  composition.provenance = static_cast<EvidenceProvenance>(*provenance);
  composition.accelerators.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    AcceleratorClassSummary summary;
    const std::optional<std::uint8_t> vendor = reader.u8();
    const std::optional<std::string> family = reader.text();
    const std::optional<bool> has_count = reader.boolean();
    const std::optional<std::uint32_t> device_count = reader.u32();
    const std::optional<bool> has_memory = reader.boolean();
    const std::optional<std::uint64_t> device_memory = reader.u64();
    if (!reader.ok() || !vendor.has_value() || !family.has_value() || !has_count.has_value() ||
        !device_count.has_value() || !has_memory.has_value() || !device_memory.has_value()) {
      return false;
    }
    if (*vendor > static_cast<std::uint8_t>(AcceleratorVendor::Other)) {
      reader.mark_invalid();
      return false;
    }
    summary.vendor = static_cast<AcceleratorVendor>(*vendor);
    summary.family = *family;
    if (*has_count) {
      summary.device_count = *device_count;
    }
    if (*has_memory) {
      summary.device_memory_bytes = *device_memory;
    }
    composition.accelerators.push_back(std::move(summary));
  }
  auto read_optional_u32 = [&reader](std::optional<std::uint32_t>& target) {
    const std::optional<bool> present = reader.boolean();
    const std::optional<std::uint32_t> value = reader.u32();
    if (!reader.ok() || !present.has_value() || !value.has_value()) {
      return false;
    }
    if (*present) {
      target = *value;
    }
    return true;
  };
  auto read_optional_u64 = [&reader](std::optional<std::uint64_t>& target) {
    const std::optional<bool> present = reader.boolean();
    const std::optional<std::uint64_t> value = reader.u64();
    if (!reader.ok() || !present.has_value() || !value.has_value()) {
      return false;
    }
    if (*present) {
      target = *value;
    }
    return true;
  };
  if (!read_optional_u32(composition.cpu_sockets) ||
      !read_optional_u32(composition.cpu_cores) ||
      !read_optional_u64(composition.host_memory_bytes) ||
      !read_optional_u32(composition.nic_count) ||
      !read_optional_u32(composition.switch_count)) {
    return false;
  }
  const std::optional<std::string> label = reader.text();
  if (!reader.ok() || !label.has_value()) {
    return false;
  }
  composition.composition_label = *label;
  return true;
}

void write_endpoints(ByteWriter& writer, const std::vector<RackEndpoint>& endpoints) {
  writer.u32(static_cast<std::uint32_t>(endpoints.size()));
  for (const RackEndpoint& endpoint : endpoints) {
    writer.text(endpoint.id.view());
    writer.optional_present(endpoint.network_domain.has_value());
    if (endpoint.network_domain.has_value()) {
      writer.text(endpoint.network_domain->view());
    }
    writer.optional_present(endpoint.link_domain.has_value());
    if (endpoint.link_domain.has_value()) {
      writer.text(endpoint.link_domain->view());
    }
    writer.u8(static_cast<std::uint8_t>(endpoint.connectivity));
    write_stamp(writer, endpoint.evidence);
  }
}

[[nodiscard]] bool read_endpoints(ByteReader& reader, std::vector<RackEndpoint>& endpoints) {
  const std::optional<std::uint32_t> count = reader.u32();
  if (!reader.ok() || !count.has_value()) {
    return false;
  }
  if (*count > kMaxRackEndpoints) {
    reader.mark_invalid();
    return false;
  }
  endpoints.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    RackEndpoint endpoint;
    const std::optional<std::string> id = reader.text();
    const std::optional<bool> has_network = reader.boolean();
    if (!reader.ok() || !id.has_value() || !has_network.has_value()) {
      return false;
    }
    const std::optional<RackEndpointId> parsed = RackEndpointId::parse(*id);
    if (!parsed.has_value()) {
      reader.mark_invalid();
      return false;
    }
    endpoint.id = *parsed;
    if (*has_network) {
      const std::optional<std::string> network = reader.text();
      if (!network.has_value()) {
        return false;
      }
      const std::optional<NetworkDomainId> domain = NetworkDomainId::parse(*network);
      if (!domain.has_value()) {
        reader.mark_invalid();
        return false;
      }
      endpoint.network_domain = *domain;
    }
    const std::optional<bool> has_link = reader.boolean();
    if (!reader.ok() || !has_link.has_value()) {
      return false;
    }
    if (*has_link) {
      const std::optional<std::string> link = reader.text();
      if (!link.has_value()) {
        return false;
      }
      const std::optional<LinkDomainId> domain = LinkDomainId::parse(*link);
      if (!domain.has_value()) {
        reader.mark_invalid();
        return false;
      }
      endpoint.link_domain = *domain;
    }
    const std::optional<std::uint8_t> connectivity = reader.u8();
    if (!reader.ok() || !connectivity.has_value() ||
        *connectivity > static_cast<std::uint8_t>(ConnectivityClass::ManagementOnly)) {
      reader.mark_invalid();
      return false;
    }
    endpoint.connectivity = static_cast<ConnectivityClass>(*connectivity);
    if (!read_stamp(reader, endpoint.evidence)) {
      return false;
    }
    endpoints.push_back(std::move(endpoint));
  }
  return true;
}

void write_hints(ByteWriter& writer, const std::vector<RackFailureDomainHint>& hints) {
  writer.u32(static_cast<std::uint32_t>(hints.size()));
  for (const RackFailureDomainHint& hint : hints) {
    writer.u8(static_cast<std::uint8_t>(hint.klass));
    writer.text(hint.id.view());
    write_stamp(writer, hint.evidence);
  }
}

[[nodiscard]] bool read_hints(ByteReader& reader, std::vector<RackFailureDomainHint>& hints) {
  const std::optional<std::uint32_t> count = reader.u32();
  if (!reader.ok() || !count.has_value()) {
    return false;
  }
  if (*count > kMaxFailureDomainRefs) {
    reader.mark_invalid();
    return false;
  }
  hints.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    RackFailureDomainHint hint;
    const std::optional<std::uint8_t> klass = reader.u8();
    const std::optional<std::string> id = reader.text();
    if (!reader.ok() || !klass.has_value() || !id.has_value()) {
      return false;
    }
    if (*klass > static_cast<std::uint8_t>(FailureDomainClass::ControlPlane)) {
      reader.mark_invalid();
      return false;
    }
    const std::optional<FailureDomainId> parsed = FailureDomainId::parse(*id);
    if (!parsed.has_value()) {
      reader.mark_invalid();
      return false;
    }
    hint.klass = static_cast<FailureDomainClass>(*klass);
    hint.id = *parsed;
    if (!read_stamp(reader, hint.evidence)) {
      return false;
    }
    hints.push_back(std::move(hint));
  }
  return true;
}

void write_readiness(ByteWriter& writer, const ReadinessContract& contract) {
  writer.u64(static_cast<std::uint64_t>(contract.minimum_current_racks));
  write_rack_ids(writer, contract.mandatory_racks);
  writer.boolean(contract.require_all_active_racks_current);
  writer.boolean(contract.require_connectivity_evidence);
  writer.u64(static_cast<std::uint64_t>(contract.minimum_current_links));
  writer.u32(static_cast<std::uint32_t>(contract.required_placement_domain_classes.size()));
  for (PlacementDomainClass klass : contract.required_placement_domain_classes) {
    writer.u8(static_cast<std::uint8_t>(klass));
  }
  writer.u32(static_cast<std::uint32_t>(contract.required_failure_domain_classes.size()));
  for (FailureDomainClass klass : contract.required_failure_domain_classes) {
    writer.u8(static_cast<std::uint8_t>(klass));
  }
  writer.boolean(contract.allow_partial);
  writer.boolean(contract.allow_degraded);
  writer.boolean(contract.require_no_conflicts);
}

[[nodiscard]] bool read_readiness(ByteReader& reader, ReadinessContract& contract) {
  const std::optional<std::uint64_t> minimum = reader.u64();
  if (!reader.ok() || !minimum.has_value() || *minimum > kMaxRacksPerCluster) {
    reader.mark_invalid();
    return false;
  }
  contract.minimum_current_racks = static_cast<std::size_t>(*minimum);
  if (!read_rack_ids(reader, contract.mandatory_racks, kMaxRacksPerCluster)) {
    return false;
  }
  const std::optional<bool> all_current = reader.boolean();
  const std::optional<bool> connectivity = reader.boolean();
  const std::optional<std::uint64_t> minimum_links = reader.u64();
  if (!reader.ok() || !all_current.has_value() || !connectivity.has_value() ||
      !minimum_links.has_value() || *minimum_links > kMaxInterRackLinks) {
    reader.mark_invalid();
    return false;
  }
  contract.require_all_active_racks_current = *all_current;
  contract.require_connectivity_evidence = *connectivity;
  contract.minimum_current_links = static_cast<std::size_t>(*minimum_links);
  const std::optional<std::uint32_t> placement_count = reader.u32();
  if (!reader.ok() || !placement_count.has_value() || *placement_count > kMaxDomainsPerClass) {
    reader.mark_invalid();
    return false;
  }
  for (std::uint32_t i = 0; i < *placement_count; ++i) {
    const std::optional<std::uint8_t> klass = reader.u8();
    if (!reader.ok() || !klass.has_value() ||
        *klass > static_cast<std::uint8_t>(PlacementDomainClass::PolicyClass)) {
      reader.mark_invalid();
      return false;
    }
    contract.required_placement_domain_classes.push_back(
        static_cast<PlacementDomainClass>(*klass));
  }
  const std::optional<std::uint32_t> failure_count = reader.u32();
  if (!reader.ok() || !failure_count.has_value() || *failure_count > kMaxDomainsPerClass) {
    reader.mark_invalid();
    return false;
  }
  for (std::uint32_t i = 0; i < *failure_count; ++i) {
    const std::optional<std::uint8_t> klass = reader.u8();
    if (!reader.ok() || !klass.has_value() ||
        *klass > static_cast<std::uint8_t>(FailureDomainClass::ControlPlane)) {
      reader.mark_invalid();
      return false;
    }
    contract.required_failure_domain_classes.push_back(static_cast<FailureDomainClass>(*klass));
  }
  const std::optional<bool> partial = reader.boolean();
  const std::optional<bool> degraded = reader.boolean();
  const std::optional<bool> conflicts = reader.boolean();
  if (!reader.ok() || !partial.has_value() || !degraded.has_value() || !conflicts.has_value()) {
    return false;
  }
  contract.allow_partial = *partial;
  contract.allow_degraded = *degraded;
  contract.require_no_conflicts = *conflicts;
  return true;
}

void write_domain_generations(ByteWriter& writer, const DomainGenerations& generations) {
  writer.u64(generations.placement.value());
  writer.u64(generations.capacity.value());
  writer.u64(generations.failure.value());
  writer.u64(generations.network.value());
  writer.u64(generations.storage.value());
  writer.u64(generations.power.value());
  writer.u64(generations.cooling.value());
  writer.u64(generations.link.value());
}

[[nodiscard]] bool read_domain_generations(ByteReader& reader, DomainGenerations& generations) {
  std::uint64_t values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (std::uint64_t& value : values) {
    const std::optional<std::uint64_t> read = reader.u64();
    if (!reader.ok() || !read.has_value()) {
      return false;
    }
    value = *read;
  }
  generations.placement = DomainGeneration::from_raw(values[0]);
  generations.capacity = DomainGeneration::from_raw(values[1]);
  generations.failure = DomainGeneration::from_raw(values[2]);
  generations.network = DomainGeneration::from_raw(values[3]);
  generations.storage = DomainGeneration::from_raw(values[4]);
  generations.power = DomainGeneration::from_raw(values[5]);
  generations.cooling = DomainGeneration::from_raw(values[6]);
  generations.link = DomainGeneration::from_raw(values[7]);
  return true;
}

}  // namespace

EncodeOutcome encode_persisted_state(const PersistedState& state) {
  EncodeOutcome outcome;
  ByteWriter writer(4096);

  writer.text(state.id.view());
  writer.u64(state.epoch.value());
  writer.u64(state.last_coordinator_epoch.value());
  writer.u64(state.generation.value());
  writer.u64(state.membership_generation.value());
  writer.u64(state.topology_epoch.value());
  writer.u64(state.topology_generation.value());
  writer.u64(state.connectivity_generation.value());
  writer.u64(state.health_generation.value());
  writer.u64(state.constraint_generation.value());
  write_domain_generations(writer, state.domain_generations);
  writer.u64(state.snapshot_generation.value());
  writer.u64(state.publication_generation.value());
  writer.u8(static_cast<std::uint8_t>(state.lifecycle));
  write_readiness(writer, state.readiness_contract);
  writer.u64(state.topology_record.epoch.value());
  writer.u64(state.topology_record.generation.value());
  writer.i64(state.topology_record.established_at.millis());
  write_stamp(writer, state.topology_record.evidence);
  writer.text(state.topology_record.reason);
  writer.i64(state.declared_at.millis());
  writer.i64(state.last_mutation_at.millis());

  writer.u32(static_cast<std::uint32_t>(state.racks.size()));
  for (const PersistedState::DurableRack& rack : state.racks) {
    writer.text(rack.rack.view());
    writer.u64(rack.generation.value());
    writer.u8(static_cast<std::uint8_t>(rack.membership));
    writer.text(rack.publisher.view());
    writer.u64(rack.last_accepted_publication.value());
    writer.text(rack.last_boot.view());
    write_composition(writer, rack.composition);
    write_endpoints(writer, rack.endpoints);
    write_hints(writer, rack.failure_domain_hints);
    writer.u8(static_cast<std::uint8_t>(rack.rack_lifecycle));
    writer.u64(rack.membership_generation.value());
    writer.text(rack.origin_label);
    writer.i64(rack.declared_at.millis());
  }

  writer.u32(static_cast<std::uint32_t>(state.placement_domains.size()));
  for (const PlacementDomain& domain : state.placement_domains) {
    writer.text(domain.id.view());
    writer.u8(static_cast<std::uint8_t>(domain.klass));
    write_rack_ids(writer, domain.racks);
    write_header(writer, domain.header);
  }

  writer.u32(static_cast<std::uint32_t>(state.capacity_domains.size()));
  for (const CapacityDomain& domain : state.capacity_domains) {
    writer.text(domain.id.view());
    writer.u8(static_cast<std::uint8_t>(domain.klass));
    write_rack_ids(writer, domain.racks);
    writer.u32(static_cast<std::uint32_t>(domain.quantities.size()));
    for (const CapacityQuantity& quantity : domain.quantities) {
      writer.text(quantity.unit);
      writer.optional_present(quantity.value.has_value());
      writer.f64(quantity.value.value_or(0.0));
      writer.u8(static_cast<std::uint8_t>(quantity.provenance));
      writer.boolean(quantity.aggregated);
    }
    write_header(writer, domain.header);
  }

  writer.u32(static_cast<std::uint32_t>(state.failure_domains.size()));
  for (const FailureDomain& domain : state.failure_domains) {
    writer.text(domain.id.view());
    writer.u8(static_cast<std::uint8_t>(domain.klass));
    write_rack_ids(writer, domain.racks);
    write_header(writer, domain.header);
  }

  writer.u32(static_cast<std::uint32_t>(state.network_domains.size()));
  for (const NetworkDomain& domain : state.network_domains) {
    writer.text(domain.id.view());
    writer.u8(static_cast<std::uint8_t>(domain.connectivity));
    write_rack_ids(writer, domain.racks);
    write_header(writer, domain.header);
  }

  writer.u32(static_cast<std::uint32_t>(state.storage_domains.size()));
  for (const StorageDomain& domain : state.storage_domains) {
    writer.text(domain.id.view());
    write_rack_ids(writer, domain.racks);
    write_header(writer, domain.header);
  }

  writer.u32(static_cast<std::uint32_t>(state.power_domains.size()));
  for (const PowerDomain& domain : state.power_domains) {
    writer.text(domain.id.view());
    write_rack_ids(writer, domain.racks);
    writer.optional_present(domain.parent.has_value());
    if (domain.parent.has_value()) {
      writer.text(domain.parent->view());
    }
    write_header(writer, domain.header);
  }

  writer.u32(static_cast<std::uint32_t>(state.cooling_domains.size()));
  for (const CoolingDomain& domain : state.cooling_domains) {
    writer.text(domain.id.view());
    write_rack_ids(writer, domain.racks);
    writer.optional_present(domain.parent.has_value());
    if (domain.parent.has_value()) {
      writer.text(domain.parent->view());
    }
    write_header(writer, domain.header);
  }

  writer.u32(static_cast<std::uint32_t>(state.link_domains.size()));
  for (const LinkDomain& domain : state.link_domains) {
    writer.text(domain.id.view());
    write_rack_ids(writer, domain.racks);
    write_header(writer, domain.header);
  }

  writer.u32(static_cast<std::uint32_t>(state.links.size()));
  for (const InterRackLink& link : state.links) {
    writer.text(link.id.view());
    writer.text(link.source.view());
    writer.text(link.destination.view());
    writer.u8(static_cast<std::uint8_t>(link.direction));
    writer.u8(static_cast<std::uint8_t>(link.connectivity));
    writer.optional_present(link.network_domain.has_value());
    if (link.network_domain.has_value()) {
      writer.text(link.network_domain->view());
    }
    writer.optional_present(link.link_domain.has_value());
    if (link.link_domain.has_value()) {
      writer.text(link.link_domain->view());
    }
    writer.optional_present(link.source_endpoint.has_value());
    if (link.source_endpoint.has_value()) {
      writer.text(link.source_endpoint->view());
    }
    writer.optional_present(link.destination_endpoint.has_value());
    if (link.destination_endpoint.has_value()) {
      writer.text(link.destination_endpoint->view());
    }
    writer.u8(static_cast<std::uint8_t>(link.bandwidth_class));
    writer.optional_present(link.nominal_bandwidth_bps.has_value());
    writer.u64(link.nominal_bandwidth_bps.value_or(0));
    writer.u8(static_cast<std::uint8_t>(link.latency_class));
    writer.optional_present(link.nominal_latency_nanos.has_value());
    writer.u64(link.nominal_latency_nanos.value_or(0));
    writer.optional_present(link.hop_count.has_value());
    writer.u32(link.hop_count.value_or(0));
    writer.u8(static_cast<std::uint8_t>(link.reachability));
    writer.u8(static_cast<std::uint8_t>(link.health));
    writer.u32(static_cast<std::uint32_t>(link.failure_domains.size()));
    for (const FailureDomainId& domain : link.failure_domains) {
      writer.text(domain.view());
    }
    write_header(writer, link.header);
    writer.u64(link.topology_epoch.value());
    writer.u64(link.topology_generation.value());
    writer.u64(link.publication.value());
    writer.optional_present(link.boot.has_value());
    if (link.boot.has_value()) {
      writer.text(link.boot->view());
    }
  }

  writer.u32(static_cast<std::uint32_t>(state.constraints.size()));
  for (const ClusterConstraint& constraint : state.constraints) {
    writer.text(constraint.id.view());
    writer.u8(static_cast<std::uint8_t>(constraint.kind));
    write_rack_ids(writer, constraint.racks);
    writer.text(constraint.domain_ref);
    writer.text(constraint.statement);
    write_header(writer, constraint.header);
  }

  writer.u32(static_cast<std::uint32_t>(state.retired_racks.size()));
  for (const RetiredIdentity& retired : state.retired_racks) {
    writer.text(retired.rack.view());
    writer.u64(retired.last_generation.value());
    writer.i64(retired.retired_at.millis());
    writer.text(retired.reason);
  }

  write_rack_ids(writer, state.withdrawn_racks);

  writer.u32(static_cast<std::uint32_t>(state.fenced_authorities.size()));
  for (const FencedAuthority& fenced : state.fenced_authorities) {
    writer.text(fenced.boot.view());
    writer.text(fenced.rack.view());
    writer.text(fenced.reason);
    writer.i64(fenced.fenced_at.millis());
  }

  if (!writer.ok()) {
    outcome.status = writer.status() == ProtocolStatus::Ok ? PersistenceStatus::InvalidState
                                                          : PersistenceStatus::BoundsExceeded;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Encode,
                                          std::string(to_string(outcome.status)),
                                          "durable state could not be encoded within bounds");
    return outcome;
  }

  const std::string payload = writer.data();
  if (payload.size() > kMaxPersistenceBytes - kContainerHeaderBytes) {
    outcome.status = PersistenceStatus::PayloadTooLarge;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Encode,
                                          "payload_too_large",
                                          "encoded durable state exceeds the container limit");
    return outcome;
  }

  std::string container;
  container.resize(kContainerHeaderBytes);
  const std::uint32_t magic = kPersistenceMagic;
  const std::uint16_t version = kPersistenceFormatVersion;
  const std::uint16_t flags = 0;
  const std::uint32_t length = static_cast<std::uint32_t>(payload.size());
  std::memcpy(container.data() + 0, &magic, 4);
  std::memcpy(container.data() + 4, &version, 2);
  std::memcpy(container.data() + 6, &flags, 2);
  std::memcpy(container.data() + 8, &length, 4);
  container += payload;
  const std::uint32_t checksum = crc32(std::string_view(container).substr(0, 12));
  const std::uint32_t payload_checksum = crc32(payload);
  const std::uint32_t combined = checksum ^ payload_checksum;
  std::memcpy(container.data() + 12, &combined, 4);

  outcome.status = PersistenceStatus::Ok;
  outcome.bytes = std::move(container);
  return outcome;
}

PersistedDecodeOutcome decode_persisted_state(std::string_view bytes) {
  PersistedDecodeOutcome outcome;
  if (bytes.empty()) {
    outcome.status = PersistenceStatus::EmptyInput;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "empty_input", "container is empty");
    return outcome;
  }
  if (bytes.size() < kContainerHeaderBytes) {
    outcome.status = PersistenceStatus::TruncatedHeader;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_header",
                                          "container is shorter than the header");
    return outcome;
  }
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t flags = 0;
  std::uint32_t length = 0;
  std::uint32_t checksum = 0;
  std::memcpy(&magic, bytes.data() + 0, 4);
  std::memcpy(&version, bytes.data() + 4, 2);
  std::memcpy(&flags, bytes.data() + 6, 2);
  std::memcpy(&length, bytes.data() + 8, 4);
  std::memcpy(&checksum, bytes.data() + 12, 4);
  if (magic != kPersistenceMagic) {
    outcome.status = PersistenceStatus::BadMagic;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "bad_magic", "container magic does not match");
    return outcome;
  }
  if (version != kPersistenceFormatVersion) {
    outcome.status = PersistenceStatus::UnsupportedVersion;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "unsupported_version",
                                          "format version " + std::to_string(version) +
                                              " is not supported by this build");
    return outcome;
  }
  if (flags != 0) {
    outcome.status = PersistenceStatus::InvalidState;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "unsupported_flags", "reserved header flags are set");
    return outcome;
  }
  const std::uint64_t declared = static_cast<std::uint64_t>(length);
  const std::uint64_t available = static_cast<std::uint64_t>(bytes.size() - kContainerHeaderBytes);
  if (declared > kMaxPersistenceBytes) {
    outcome.status = PersistenceStatus::PayloadTooLarge;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "payload_too_large",
                                          "declared payload exceeds the container limit");
    return outcome;
  }
  if (declared > available) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body",
                                          "declared payload length exceeds the container content");
    return outcome;
  }
  if (declared < available) {
    outcome.status = PersistenceStatus::TrailingGarbage;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "trailing_garbage",
                                          "container has " + std::to_string(available - declared) +
                                              " bytes after the declared payload");
    return outcome;
  }
  const std::string_view payload = bytes.substr(kContainerHeaderBytes, length);
  const std::uint32_t header_checksum = crc32(bytes.substr(0, 12));
  const std::uint32_t payload_checksum = crc32(payload);
  if ((header_checksum ^ payload_checksum) != checksum) {
    outcome.status = PersistenceStatus::ChecksumMismatch;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "checksum_mismatch",
                                          "container checksum does not match its content");
    return outcome;
  }

  PersistedState state;
  ByteReader reader(payload);

  const std::optional<std::string> cluster = reader.text();
  if (!cluster.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "cluster identity is missing");
    return outcome;
  }
  const std::optional<ClusterId> parsed_cluster = ClusterId::parse(*cluster);
  if (!parsed_cluster.has_value()) {
    outcome.status = PersistenceStatus::InvalidIdentity;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "invalid_identity", "cluster identity is not valid");
    return outcome;
  }
  state.id = *parsed_cluster;

  auto read_u64 = [&reader](std::uint64_t& target) {
    const std::optional<std::uint64_t> value = reader.u64();
    if (!reader.ok() || !value.has_value()) {
      return false;
    }
    target = *value;
    return true;
  };
  std::uint64_t epoch = 0;
  std::uint64_t coordinator_epoch = 0;
  std::uint64_t generation = 0;
  std::uint64_t membership_generation = 0;
  std::uint64_t topology_epoch = 0;
  std::uint64_t topology_generation = 0;
  std::uint64_t connectivity_generation = 0;
  std::uint64_t health_generation = 0;
  std::uint64_t constraint_generation = 0;
  std::uint64_t snapshot_generation = 0;
  std::uint64_t publication_generation = 0;
  if (!read_u64(epoch) || !read_u64(coordinator_epoch) || !read_u64(generation) ||
      !read_u64(membership_generation) || !read_u64(topology_epoch) ||
      !read_u64(topology_generation) || !read_u64(connectivity_generation) ||
      !read_u64(health_generation) || !read_u64(constraint_generation) ||
      !read_domain_generations(reader, state.domain_generations) ||
      !read_u64(snapshot_generation) || !read_u64(publication_generation)) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "generation block is incomplete");
    return outcome;
  }
  state.epoch = ClusterEpoch::from_raw(epoch);
  state.last_coordinator_epoch = CoordinatorEpoch::from_raw(coordinator_epoch);
  state.generation = ClusterGeneration::from_raw(generation);
  state.membership_generation = MembershipGeneration::from_raw(membership_generation);
  state.topology_epoch = TopologyEpoch::from_raw(topology_epoch);
  state.topology_generation = TopologyGeneration::from_raw(topology_generation);
  state.connectivity_generation = ConnectivityGeneration::from_raw(connectivity_generation);
  state.health_generation = HealthGeneration::from_raw(health_generation);
  state.constraint_generation = ConstraintGeneration::from_raw(constraint_generation);
  state.snapshot_generation = SnapshotGeneration::from_raw(snapshot_generation);
  state.publication_generation = PublicationGeneration::from_raw(publication_generation);

  const std::optional<std::uint8_t> lifecycle = reader.u8();
  if (!reader.ok() || !lifecycle.has_value() ||
      *lifecycle > static_cast<std::uint8_t>(ClusterLifecycle::Retired)) {
    outcome.status = PersistenceStatus::InvalidEnum;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "invalid_enum", "lifecycle value is out of range");
    return outcome;
  }
  state.lifecycle = static_cast<ClusterLifecycle>(*lifecycle);

  if (!read_readiness(reader, state.readiness_contract)) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "readiness contract is incomplete");
    return outcome;
  }

  // Field order MUST mirror encode_state exactly: epoch, generation,
  // established_at, evidence stamp, reason.
  std::uint64_t topology_record_epoch = 0;
  std::uint64_t topology_record_generation = 0;
  if (!read_u64(topology_record_epoch) || !read_u64(topology_record_generation)) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "topology record is incomplete");
    return outcome;
  }
  const std::optional<std::int64_t> topology_record_at = reader.i64();
  if (!reader.ok() || !topology_record_at.has_value() ||
      !read_stamp(reader, state.topology_record.evidence)) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "topology record is incomplete");
    return outcome;
  }
  const std::optional<std::string> topology_reason = reader.text();
  const std::optional<std::int64_t> declared_at = reader.i64();
  const std::optional<std::int64_t> last_mutation_at = reader.i64();
  if (!reader.ok() || !topology_reason.has_value() || !declared_at.has_value() ||
      !last_mutation_at.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "timestamp block is incomplete");
    return outcome;
  }
  state.topology_record.epoch = TopologyEpoch::from_raw(topology_record_epoch);
  state.topology_record.generation = TopologyGeneration::from_raw(topology_record_generation);
  state.topology_record.established_at = Timestamp::from_unix_millis(*topology_record_at);
  state.topology_record.reason = *topology_reason;
  state.declared_at = Timestamp::from_unix_millis(*declared_at);
  state.last_mutation_at = Timestamp::from_unix_millis(*last_mutation_at);

  const std::optional<std::uint32_t> rack_count = reader.u32();
  if (!reader.ok() || !rack_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "rack block is missing");
    return outcome;
  }
  if (*rack_count > kMaxRacksPerCluster) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "rack count exceeds the bound");
    return outcome;
  }
  state.racks.reserve(*rack_count);
  for (std::uint32_t i = 0; i < *rack_count; ++i) {
    PersistedState::DurableRack rack;
    const std::optional<std::string> rack_id = reader.text();
    const std::optional<std::uint64_t> rack_generation = reader.u64();
    const std::optional<std::uint8_t> membership = reader.u8();
    const std::optional<std::string> publisher = reader.text();
    const std::optional<std::uint64_t> publication = reader.u64();
    const std::optional<std::string> last_boot = reader.text();
    if (!reader.ok() || !rack_id.has_value() || !rack_generation.has_value() ||
        !membership.has_value() || !publisher.has_value() || !publication.has_value() ||
        !last_boot.has_value()) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "rack record is incomplete");
      return outcome;
    }
    const std::optional<RackId> parsed_rack = RackId::parse(*rack_id);
    const std::optional<RackPublisherId> parsed_publisher = RackPublisherId::parse(*publisher);
    const std::optional<RackAgentBootId> parsed_boot = RackAgentBootId::parse(*last_boot);
    if (!parsed_rack.has_value() || !parsed_publisher.has_value() || !parsed_boot.has_value()) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_identity", "rack identity is not valid");
      return outcome;
    }
    if (*membership > static_cast<std::uint8_t>(RackMembershipState::Retired)) {
      outcome.status = PersistenceStatus::InvalidEnum;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_enum", "membership value is out of range");
      return outcome;
    }
    rack.rack = *parsed_rack;
    rack.generation = RackGeneration::from_raw(*rack_generation);
    rack.membership = static_cast<RackMembershipState>(*membership);
    rack.publisher = *parsed_publisher;
    rack.last_accepted_publication = PublicationGeneration::from_raw(*publication);
    rack.last_boot = *parsed_boot;
    if (!read_composition(reader, rack.composition) || !read_endpoints(reader, rack.endpoints) ||
        !read_hints(reader, rack.failure_domain_hints)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body",
                                            "rack composition block is incomplete");
      return outcome;
    }
    const std::optional<std::uint8_t> rack_lifecycle = reader.u8();
    const std::optional<std::uint64_t> rack_membership_generation = reader.u64();
    const std::optional<std::string> origin = reader.text();
    const std::optional<std::int64_t> rack_declared = reader.i64();
    if (!reader.ok() || !rack_lifecycle.has_value() || !rack_membership_generation.has_value() ||
        !origin.has_value() || !rack_declared.has_value()) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "rack tail block is incomplete");
      return outcome;
    }
    if (*rack_lifecycle > static_cast<std::uint8_t>(RackLifecycleState::Retired)) {
      outcome.status = PersistenceStatus::InvalidEnum;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_enum", "rack lifecycle is out of range");
      return outcome;
    }
    rack.rack_lifecycle = static_cast<RackLifecycleState>(*rack_lifecycle);
    rack.membership_generation = MembershipGeneration::from_raw(*rack_membership_generation);
    rack.origin_label = *origin;
    rack.declared_at = Timestamp::from_unix_millis(*rack_declared);
    state.racks.push_back(std::move(rack));
  }

  const std::optional<std::uint32_t> placement_count = reader.u32();
  if (!reader.ok() || !placement_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "placement domain block is missing");
    return outcome;
  }
  if (*placement_count > kMaxDomainsPerClass) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "placement domain count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *placement_count; ++i) {
    PlacementDomain domain;
    const std::optional<std::string> id = reader.text();
    const std::optional<std::uint8_t> klass = reader.u8();
    if (!reader.ok() || !id.has_value() || !klass.has_value()) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "placement domain is incomplete");
      return outcome;
    }
    const std::optional<PlacementDomainId> parsed = PlacementDomainId::parse(*id);
    if (!parsed.has_value()) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_identity",
                                            "placement domain identity is not valid");
      return outcome;
    }
    if (*klass > static_cast<std::uint8_t>(PlacementDomainClass::PolicyClass)) {
      outcome.status = PersistenceStatus::InvalidEnum;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_enum", "placement domain class is out of range");
      return outcome;
    }
    domain.id = *parsed;
    domain.klass = static_cast<PlacementDomainClass>(*klass);
    if (!read_rack_ids(reader, domain.racks, kMaxDomainMembers) ||
        !read_header(reader, domain.header)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "placement domain body is incomplete");
      return outcome;
    }
    state.placement_domains.push_back(std::move(domain));
  }

  const std::optional<std::uint32_t> capacity_count = reader.u32();
  if (!reader.ok() || !capacity_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "capacity domain block is missing");
    return outcome;
  }
  if (*capacity_count > kMaxDomainsPerClass) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "capacity domain count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *capacity_count; ++i) {
    CapacityDomain domain;
    const std::optional<std::string> id = reader.text();
    const std::optional<std::uint8_t> klass = reader.u8();
    if (!reader.ok() || !id.has_value() || !klass.has_value()) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "capacity domain is incomplete");
      return outcome;
    }
    const std::optional<CapacityDomainId> parsed = CapacityDomainId::parse(*id);
    if (!parsed.has_value()) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_identity",
                                            "capacity domain identity is not valid");
      return outcome;
    }
    if (*klass > static_cast<std::uint8_t>(CapacityDomainClass::StorageDomain)) {
      outcome.status = PersistenceStatus::InvalidEnum;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_enum", "capacity domain class is out of range");
      return outcome;
    }
    domain.id = *parsed;
    domain.klass = static_cast<CapacityDomainClass>(*klass);
    if (!read_rack_ids(reader, domain.racks, kMaxDomainMembers)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "capacity domain members missing");
      return outcome;
    }
    const std::optional<std::uint32_t> quantity_count = reader.u32();
    if (!reader.ok() || !quantity_count.has_value()) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "capacity quantity block missing");
      return outcome;
    }
    if (*quantity_count > kMaxCapacityQuantities) {
      outcome.status = PersistenceStatus::AbsurdCount;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "absurd_count", "capacity quantity count exceeds bound");
      return outcome;
    }
    for (std::uint32_t q = 0; q < *quantity_count; ++q) {
      CapacityQuantity quantity;
      const std::optional<std::string> unit = reader.text();
      const std::optional<bool> present = reader.boolean();
      const std::optional<double> value = reader.f64();
      const std::optional<std::uint8_t> provenance = reader.u8();
      const std::optional<bool> aggregated = reader.boolean();
      if (!reader.ok() || !unit.has_value() || !present.has_value() || !value.has_value() ||
          !provenance.has_value() || !aggregated.has_value()) {
        outcome.status = PersistenceStatus::TruncatedBody;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                              "truncated_body", "capacity quantity is incomplete");
        return outcome;
      }
      if (*provenance > static_cast<std::uint8_t>(EvidenceProvenance::Reconstructed)) {
        outcome.status = PersistenceStatus::InvalidEnum;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                              "invalid_enum", "quantity provenance out of range");
        return outcome;
      }
      quantity.unit = *unit;
      if (*present) {
        quantity.value = *value;
      }
      quantity.provenance = static_cast<EvidenceProvenance>(*provenance);
      quantity.aggregated = *aggregated;
      domain.quantities.push_back(std::move(quantity));
    }
    if (!read_header(reader, domain.header)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "capacity domain header incomplete");
      return outcome;
    }
    state.capacity_domains.push_back(std::move(domain));
  }

  auto read_enum = [&reader](auto& target, std::uint8_t max_value) {
    const std::optional<std::uint8_t> value = reader.u8();
    if (!reader.ok() || !value.has_value() || *value > max_value) {
      return false;
    }
    target = static_cast<std::decay_t<decltype(target)>>(*value);
    return true;
  };
  auto read_optional_text = [&reader](std::optional<std::string>& target) {
    const std::optional<bool> present = reader.boolean();
    if (!reader.ok() || !present.has_value()) {
      return false;
    }
    if (!*present) {
      return true;
    }
    const std::optional<std::string> value = reader.text();
    if (!value.has_value()) {
      return false;
    }
    target = *value;
    return true;
  };
  const std::optional<std::uint32_t> failure_count = reader.u32();
  if (!reader.ok() || !failure_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "failure domain block is missing");
    return outcome;
  }
  if (*failure_count > kMaxDomainsPerClass) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "failure domain count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *failure_count; ++i) {
    FailureDomain domain;
    const std::optional<std::string> id = reader.text();
    const std::optional<FailureDomainId> parsed = id.has_value() ? FailureDomainId::parse(*id)
                                                                : std::nullopt;
    if (!reader.ok() || !parsed.has_value() ||
        !read_enum(domain.klass, static_cast<std::uint8_t>(FailureDomainClass::ControlPlane))) {
      outcome.status = id.has_value() ? PersistenceStatus::InvalidIdentity
                                      : PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_failure_domain",
                                            "failure domain identity or class is not valid");
      return outcome;
    }
    domain.id = *parsed;
    if (!read_rack_ids(reader, domain.racks, kMaxDomainMembers) ||
        !read_header(reader, domain.header)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "failure domain body is incomplete");
      return outcome;
    }
    state.failure_domains.push_back(std::move(domain));
  }

  const std::optional<std::uint32_t> network_count = reader.u32();
  if (!reader.ok() || !network_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "network domain block is missing");
    return outcome;
  }
  if (*network_count > kMaxDomainsPerClass) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "network domain count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *network_count; ++i) {
    NetworkDomain domain;
    const std::optional<std::string> id = reader.text();
    const std::optional<NetworkDomainId> parsed = id.has_value() ? NetworkDomainId::parse(*id)
                                                                 : std::nullopt;
    if (!reader.ok() || !parsed.has_value() ||
        !read_enum(domain.connectivity,
                   static_cast<std::uint8_t>(ConnectivityClass::ManagementOnly))) {
      outcome.status = id.has_value() ? PersistenceStatus::InvalidIdentity
                                      : PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_network_domain",
                                            "network domain identity or connectivity is not valid");
      return outcome;
    }
    domain.id = *parsed;
    if (!read_rack_ids(reader, domain.racks, kMaxDomainMembers) ||
        !read_header(reader, domain.header)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "network domain body is incomplete");
      return outcome;
    }
    state.network_domains.push_back(std::move(domain));
  }

  const std::optional<std::uint32_t> storage_count = reader.u32();
  if (!reader.ok() || !storage_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "storage domain block is missing");
    return outcome;
  }
  if (*storage_count > kMaxDomainsPerClass) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "storage domain count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *storage_count; ++i) {
    StorageDomain domain;
    const std::optional<std::string> id = reader.text();
    const std::optional<StorageDomainId> parsed = id.has_value() ? StorageDomainId::parse(*id)
                                                                 : std::nullopt;
    if (!reader.ok() || !parsed.has_value()) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_identity",
                                            "storage domain identity is not valid");
      return outcome;
    }
    domain.id = *parsed;
    if (!read_rack_ids(reader, domain.racks, kMaxDomainMembers) ||
        !read_header(reader, domain.header)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "storage domain body is incomplete");
      return outcome;
    }
    state.storage_domains.push_back(std::move(domain));
  }

  const std::optional<std::uint32_t> power_count = reader.u32();
  if (!reader.ok() || !power_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "power domain block is missing");
    return outcome;
  }
  if (*power_count > kMaxDomainsPerClass) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "power domain count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *power_count; ++i) {
    PowerDomain domain;
    const std::optional<std::string> id = reader.text();
    const std::optional<PowerDomainId> parsed = id.has_value() ? PowerDomainId::parse(*id)
                                                               : std::nullopt;
    if (!reader.ok() || !parsed.has_value()) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_power_domain",
                                            "power domain identity or parent is not valid");
      return outcome;
    }
    domain.id = *parsed;
    // Field order mirrors encode_state: id, member racks, optional parent, header.
    if (!read_rack_ids(reader, domain.racks, kMaxDomainMembers)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "power domain body is incomplete");
      return outcome;
    }
    std::optional<std::string> parent;
    if (!read_optional_text(parent)) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_power_domain",
                                            "power domain identity or parent is not valid");
      return outcome;
    }
    if (parent.has_value()) {
      const std::optional<PowerDomainId> parsed_parent = PowerDomainId::parse(*parent);
      if (!parsed_parent.has_value()) {
        outcome.status = PersistenceStatus::InvalidIdentity;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                              "invalid_identity",
                                              "power domain parent identity is not valid");
        return outcome;
      }
      domain.parent = *parsed_parent;
    }
    if (!read_header(reader, domain.header)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "power domain body is incomplete");
      return outcome;
    }
    state.power_domains.push_back(std::move(domain));
  }

  const std::optional<std::uint32_t> cooling_count = reader.u32();
  if (!reader.ok() || !cooling_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "cooling domain block is missing");
    return outcome;
  }
  if (*cooling_count > kMaxDomainsPerClass) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "cooling domain count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *cooling_count; ++i) {
    CoolingDomain domain;
    const std::optional<std::string> id = reader.text();
    const std::optional<CoolingDomainId> parsed = id.has_value() ? CoolingDomainId::parse(*id)
                                                                 : std::nullopt;
    if (!reader.ok() || !parsed.has_value()) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_cooling_domain",
                                            "cooling domain identity or parent is not valid");
      return outcome;
    }
    domain.id = *parsed;
    // Field order mirrors encode_state: id, member racks, optional parent, header.
    if (!read_rack_ids(reader, domain.racks, kMaxDomainMembers)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "cooling domain body is incomplete");
      return outcome;
    }
    std::optional<std::string> parent;
    if (!read_optional_text(parent)) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_cooling_domain",
                                            "cooling domain identity or parent is not valid");
      return outcome;
    }
    if (parent.has_value()) {
      const std::optional<CoolingDomainId> parsed_parent = CoolingDomainId::parse(*parent);
      if (!parsed_parent.has_value()) {
        outcome.status = PersistenceStatus::InvalidIdentity;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                              "invalid_identity",
                                              "cooling domain parent identity is not valid");
        return outcome;
      }
      domain.parent = *parsed_parent;
    }
    if (!read_header(reader, domain.header)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "cooling domain body is incomplete");
      return outcome;
    }
    state.cooling_domains.push_back(std::move(domain));
  }

  const std::optional<std::uint32_t> link_domain_count = reader.u32();
  if (!reader.ok() || !link_domain_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "link domain block is missing");
    return outcome;
  }
  if (*link_domain_count > kMaxDomainsPerClass) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "link domain count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *link_domain_count; ++i) {
    LinkDomain domain;
    const std::optional<std::string> id = reader.text();
    const std::optional<LinkDomainId> parsed = id.has_value() ? LinkDomainId::parse(*id)
                                                              : std::nullopt;
    if (!reader.ok() || !parsed.has_value()) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_identity", "link domain identity is not valid");
      return outcome;
    }
    domain.id = *parsed;
    if (!read_rack_ids(reader, domain.racks, kMaxDomainMembers) ||
        !read_header(reader, domain.header)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "link domain body is incomplete");
      return outcome;
    }
    state.link_domains.push_back(std::move(domain));
  }

  const std::optional<std::uint32_t> link_count = reader.u32();
  if (!reader.ok() || !link_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "link block is missing");
    return outcome;
  }
  if (*link_count > kMaxInterRackLinks) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "link count exceeds the bound");
    return outcome;
  }
  state.links.reserve(*link_count);
  for (std::uint32_t i = 0; i < *link_count; ++i) {
    InterRackLink link;
    const std::optional<std::string> id = reader.text();
    const std::optional<std::string> source = reader.text();
    const std::optional<std::string> destination = reader.text();
    const std::optional<InterRackLinkId> parsed_id =
        id.has_value() ? InterRackLinkId::parse(*id) : std::nullopt;
    const std::optional<RackId> parsed_source = source.has_value() ? RackId::parse(*source)
                                                                   : std::nullopt;
    const std::optional<RackId> parsed_destination =
        destination.has_value() ? RackId::parse(*destination) : std::nullopt;
    if (!reader.ok() || !parsed_id.has_value() || !parsed_source.has_value() ||
        !parsed_destination.has_value() ||
        !read_enum(link.direction, static_cast<std::uint8_t>(LinkDirection::Bidirectional)) ||
        !read_enum(link.connectivity,
                   static_cast<std::uint8_t>(ConnectivityClass::ManagementOnly))) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_link",
                                            "link identity, endpoints, direction or connectivity "
                                            "is not valid");
      return outcome;
    }
    link.id = *parsed_id;
    link.source = *parsed_source;
    link.destination = *parsed_destination;

    std::optional<std::string> network_domain;
    std::optional<std::string> link_domain;
    std::optional<std::string> source_endpoint;
    std::optional<std::string> destination_endpoint;
    if (!read_optional_text(network_domain) || !read_optional_text(link_domain) ||
        !read_optional_text(source_endpoint) || !read_optional_text(destination_endpoint)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "link reference block is incomplete");
      return outcome;
    }
    if (network_domain.has_value()) {
      const std::optional<NetworkDomainId> parsed = NetworkDomainId::parse(*network_domain);
      if (!parsed.has_value()) {
        outcome.status = PersistenceStatus::InvalidIdentity;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                              "invalid_identity", "link network domain is invalid");
        return outcome;
      }
      link.network_domain = *parsed;
    }
    if (link_domain.has_value()) {
      const std::optional<LinkDomainId> parsed = LinkDomainId::parse(*link_domain);
      if (!parsed.has_value()) {
        outcome.status = PersistenceStatus::InvalidIdentity;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                              "invalid_identity", "link domain is invalid");
        return outcome;
      }
      link.link_domain = *parsed;
    }
    if (source_endpoint.has_value()) {
      const std::optional<RackEndpointId> parsed = RackEndpointId::parse(*source_endpoint);
      if (!parsed.has_value()) {
        outcome.status = PersistenceStatus::InvalidIdentity;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                              "invalid_identity", "link source endpoint is invalid");
        return outcome;
      }
      link.source_endpoint = *parsed;
    }
    if (destination_endpoint.has_value()) {
      const std::optional<RackEndpointId> parsed = RackEndpointId::parse(*destination_endpoint);
      if (!parsed.has_value()) {
        outcome.status = PersistenceStatus::InvalidIdentity;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                              "invalid_identity",
                                              "link destination endpoint is invalid");
        return outcome;
      }
      link.destination_endpoint = *parsed;
    }

    if (!read_enum(link.bandwidth_class,
                   static_cast<std::uint8_t>(BandwidthClass::VeryHigh))) {
      outcome.status = PersistenceStatus::InvalidEnum;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_enum", "bandwidth class is out of range");
      return outcome;
    }
    const std::optional<bool> has_bandwidth = reader.boolean();
    const std::optional<std::uint64_t> bandwidth = reader.u64();
    if (!reader.ok() || !has_bandwidth.has_value() || !bandwidth.has_value()) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "link bandwidth block is incomplete");
      return outcome;
    }
    if (*has_bandwidth) {
      link.nominal_bandwidth_bps = *bandwidth;
    }
    if (!read_enum(link.latency_class, static_cast<std::uint8_t>(LatencyClass::High))) {
      outcome.status = PersistenceStatus::InvalidEnum;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_enum", "latency class is out of range");
      return outcome;
    }
    const std::optional<bool> has_latency = reader.boolean();
    const std::optional<std::uint64_t> latency = reader.u64();
    if (!reader.ok() || !has_latency.has_value() || !latency.has_value()) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "link latency block is incomplete");
      return outcome;
    }
    if (*has_latency) {
      link.nominal_latency_nanos = *latency;
    }
    const std::optional<bool> has_hops = reader.boolean();
    const std::optional<std::uint32_t> hops = reader.u32();
    if (!reader.ok() || !has_hops.has_value() || !hops.has_value()) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "link hop block is incomplete");
      return outcome;
    }
    if (*has_hops) {
      link.hop_count = *hops;
    }
    if (!read_enum(link.reachability,
                   static_cast<std::uint8_t>(Reachability::RevalidationRequired)) ||
        !read_enum(link.health, static_cast<std::uint8_t>(HealthState::Unhealthy))) {
      outcome.status = PersistenceStatus::InvalidEnum;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_enum", "link reachability or health is out of "
                                                            "range");
      return outcome;
    }
    const std::optional<std::uint32_t> domain_count = reader.u32();
    if (!reader.ok() || !domain_count.has_value() || *domain_count > kMaxFailureDomainRefs) {
      outcome.status = PersistenceStatus::AbsurdCount;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "absurd_count",
                                            "link failure domain count is missing or exceeds the "
                                            "bound");
      return outcome;
    }
    link.failure_domains.reserve(*domain_count);
    for (std::uint32_t d = 0; d < *domain_count; ++d) {
      const std::optional<std::string> domain_id = reader.text();
      const std::optional<FailureDomainId> parsed =
          domain_id.has_value() ? FailureDomainId::parse(*domain_id) : std::nullopt;
      if (!parsed.has_value()) {
        outcome.status = PersistenceStatus::InvalidIdentity;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                              "invalid_identity",
                                              "link failure domain identity is not valid");
        return outcome;
      }
      link.failure_domains.push_back(*parsed);
    }
    if (!read_header(reader, link.header)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "link header is incomplete");
      return outcome;
    }
    std::uint64_t link_topology_epoch = 0;
    std::uint64_t link_topology_generation = 0;
    std::uint64_t link_publication = 0;
    if (!read_u64(link_topology_epoch) || !read_u64(link_topology_generation) ||
        !read_u64(link_publication)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "link generation block is incomplete");
      return outcome;
    }
    link.topology_epoch = TopologyEpoch::from_raw(link_topology_epoch);
    link.topology_generation = TopologyGeneration::from_raw(link_topology_generation);
    link.publication = PublicationGeneration::from_raw(link_publication);
    std::optional<std::string> boot;
    if (!read_optional_text(boot)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "link boot block is incomplete");
      return outcome;
    }
    if (boot.has_value()) {
      const std::optional<RackAgentBootId> parsed = RackAgentBootId::parse(*boot);
      if (!parsed.has_value()) {
        outcome.status = PersistenceStatus::InvalidIdentity;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                              "invalid_identity", "link boot identity is not valid");
        return outcome;
      }
      link.boot = *parsed;
    }
    state.links.push_back(std::move(link));
  }

  const std::optional<std::uint32_t> constraint_count = reader.u32();
  if (!reader.ok() || !constraint_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "constraint block is missing");
    return outcome;
  }
  if (*constraint_count > kMaxConstraints) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "constraint count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *constraint_count; ++i) {
    ClusterConstraint constraint;
    const std::optional<std::string> id = reader.text();
    const std::optional<ConstraintId> parsed = id.has_value() ? ConstraintId::parse(*id)
                                                              : std::nullopt;
    if (!reader.ok() || !parsed.has_value() ||
        !read_enum(constraint.kind, static_cast<std::uint8_t>(ConstraintKind::MaintenanceWindow)) ||
        !read_rack_ids(reader, constraint.racks, kMaxDomainMembers)) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_constraint",
                                            "constraint identity, kind or members are not valid");
      return outcome;
    }
    constraint.id = *parsed;
    const std::optional<std::string> domain_ref = reader.text();
    const std::optional<std::string> statement = reader.text();
    if (!reader.ok() || !domain_ref.has_value() || !statement.has_value() ||
        !read_header(reader, constraint.header)) {
      outcome.status = PersistenceStatus::TruncatedBody;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "truncated_body", "constraint body is incomplete");
      return outcome;
    }
    constraint.domain_ref = *domain_ref;
    constraint.statement = *statement;
    state.constraints.push_back(std::move(constraint));
  }

  const std::optional<std::uint32_t> retired_count = reader.u32();
  if (!reader.ok() || !retired_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "retired identity block is missing");
    return outcome;
  }
  if (*retired_count > kMaxRacksPerCluster) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "retired identity count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *retired_count; ++i) {
    RetiredIdentity retired;
    const std::optional<std::string> rack = reader.text();
    const std::optional<std::uint64_t> last_generation = reader.u64();
    const std::optional<std::int64_t> retired_at = reader.i64();
    const std::optional<std::string> reason = reader.text();
    const std::optional<RackId> parsed = rack.has_value() ? RackId::parse(*rack) : std::nullopt;
    if (!reader.ok() || !parsed.has_value() || !last_generation.has_value() ||
        !retired_at.has_value() || !reason.has_value()) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_retired_identity",
                                            "retired identity record is invalid");
      return outcome;
    }
    retired.rack = *parsed;
    retired.last_generation = RackGeneration::from_raw(*last_generation);
    retired.retired_at = Timestamp::from_unix_millis(*retired_at);
    retired.reason = *reason;
    state.retired_racks.push_back(std::move(retired));
  }

  if (!read_rack_ids(reader, state.withdrawn_racks, kMaxRacksPerCluster)) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "withdrawn rack block is incomplete");
    return outcome;
  }

  const std::optional<std::uint32_t> fenced_count = reader.u32();
  if (!reader.ok() || !fenced_count.has_value()) {
    outcome.status = PersistenceStatus::TruncatedBody;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "truncated_body", "fenced authority block is missing");
    return outcome;
  }
  if (*fenced_count > kMaxRacksPerCluster) {
    outcome.status = PersistenceStatus::AbsurdCount;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          "absurd_count", "fenced authority count exceeds the bound");
    return outcome;
  }
  for (std::uint32_t i = 0; i < *fenced_count; ++i) {
    FencedAuthority fenced;
    const std::optional<std::string> boot = reader.text();
    const std::optional<std::string> rack = reader.text();
    const std::optional<std::string> reason = reader.text();
    const std::optional<std::int64_t> fenced_at = reader.i64();
    const std::optional<RackAgentBootId> parsed_boot =
        boot.has_value() ? RackAgentBootId::parse(*boot) : std::nullopt;
    const std::optional<RackId> parsed_rack = rack.has_value() ? RackId::parse(*rack)
                                                               : std::nullopt;
    if (!reader.ok() || !parsed_boot.has_value() || !parsed_rack.has_value() ||
        !reason.has_value() || !fenced_at.has_value()) {
      outcome.status = PersistenceStatus::InvalidIdentity;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                            "invalid_fenced_authority",
                                            "fenced authority record is invalid");
      return outcome;
    }
    fenced.boot = *parsed_boot;
    fenced.rack = *parsed_rack;
    fenced.reason = *reason;
    fenced.fenced_at = Timestamp::from_unix_millis(*fenced_at);
    state.fenced_authorities.push_back(std::move(fenced));
  }

  if (!reader.exhausted()) {
    outcome.status = PersistenceStatus::TrailingGarbage;
    outcome.error = StructuredError::make(
        ErrorCategory::Persistence, ErrorStage::Decode, "trailing_garbage",
        std::to_string(reader.remaining()) + " unread bytes after the last record");
    return outcome;
  }

  std::string validation_detail;
  const PersistenceStatus validation = validate_persisted_state(state, &validation_detail);
  if (validation != PersistenceStatus::Ok) {
    outcome.status = validation;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Decode,
                                          std::string(to_string(validation)),
                                          validation_detail.empty()
                                              ? std::string("decoded state failed structural "
                                                            "validation")
                                              : validation_detail);
    return outcome;
  }
  outcome.status = PersistenceStatus::Ok;
  outcome.consumed = bytes.size();
  outcome.state = std::move(state);
  return outcome;
}

PersistenceStatus validate_persisted_state(const PersistedState& state, std::string* detail) {
  const auto fail = [detail](PersistenceStatus status, std::string text) {
    if (detail != nullptr) {
      *detail = std::move(text);
    }
    return status;
  };

  if (!state.id.known() || !validate_identity(state.id.view()).ok()) {
    return fail(PersistenceStatus::InvalidIdentity, "cluster identity is missing or malformed");
  }
  if (!state.epoch.known()) {
    return fail(PersistenceStatus::InvalidEpoch, "cluster epoch is zero");
  }
  if (!state.last_coordinator_epoch.known()) {
    return fail(PersistenceStatus::InvalidEpoch, "coordinator epoch is zero");
  }
  if (!state.generation.known() || !state.membership_generation.known() ||
      !state.topology_generation.known() || !state.connectivity_generation.known() ||
      !state.health_generation.known() || !state.constraint_generation.known() ||
      !state.snapshot_generation.known() || !state.publication_generation.known()) {
    return fail(PersistenceStatus::InvalidGeneration, "a top-level generation is zero");
  }
  if (!state.topology_epoch.known() && !state.topology_record.epoch.known()) {
    return fail(PersistenceStatus::InvalidEpoch, "no topology epoch is recorded");
  }
  if (state.racks.size() > kMaxRacksPerCluster) {
    return fail(PersistenceStatus::AbsurdCount,
                "rack count " + std::to_string(state.racks.size()) + " exceeds the bound");
  }
  if (state.links.size() > kMaxInterRackLinks) {
    return fail(PersistenceStatus::AbsurdCount,
                "link count " + std::to_string(state.links.size()) + " exceeds the bound");
  }
  if (state.constraints.size() > kMaxConstraints) {
    return fail(PersistenceStatus::AbsurdCount,
                "constraint count " + std::to_string(state.constraints.size()) + " exceeds the bound");
  }

  std::vector<RackId> rack_ids;
  rack_ids.reserve(state.racks.size());
  for (const PersistedState::DurableRack& rack : state.racks) {
    if (!rack.rack.known() || !validate_identity(rack.rack.view()).ok()) {
      return fail(PersistenceStatus::InvalidIdentity, "a rack identity is missing or malformed");
    }
    if (!rack.generation.known()) {
      return fail(PersistenceStatus::InvalidGeneration,
                  "rack " + std::string(rack.rack.view()) + " has a zero generation");
    }
    if (!rack.publisher.known() || !validate_identity(rack.publisher.view()).ok()) {
      return fail(PersistenceStatus::InvalidIdentity,
                  "rack " + std::string(rack.rack.view()) + " has no valid publisher");
    }
    if (!rack_ids.empty() && !(rack_ids.back() < rack.rack)) {
      return fail(PersistenceStatus::DuplicateRackIdentity,
                  "rack " + std::string(rack.rack.view()) +
                      " is duplicated or the rack list is not sorted");
    }
    rack_ids.push_back(rack.rack);

    if (rack.endpoints.size() > kMaxRackEndpoints) {
      return fail(PersistenceStatus::BoundsExceeded,
                  "rack " + std::string(rack.rack.view()) + " declares too many endpoints");
    }
    for (std::size_t i = 0; i < rack.endpoints.size(); ++i) {
      const RackEndpoint& endpoint = rack.endpoints[i];
      if (!endpoint.id.known() || !validate_identity(endpoint.id.view()).ok()) {
        return fail(PersistenceStatus::InvalidIdentity,
                    "rack " + std::string(rack.rack.view()) + " has a malformed endpoint identity");
      }
      if (i > 0 && !(rack.endpoints[i - 1].id < endpoint.id)) {
        return fail(PersistenceStatus::InvalidState,
                    "rack " + std::string(rack.rack.view()) +
                        " endpoints are duplicated or not sorted");
      }
    }
    if (rack.failure_domain_hints.size() > kMaxFailureDomainRefs) {
      return fail(PersistenceStatus::BoundsExceeded,
                  "rack " + std::string(rack.rack.view()) + " declares too many failure domains");
    }
    for (std::size_t i = 0; i < rack.failure_domain_hints.size(); ++i) {
      const RackFailureDomainHint& hint = rack.failure_domain_hints[i];
      if (hint.klass == FailureDomainClass::Unknown) {
        return fail(PersistenceStatus::InvalidEnum,
                    "rack " + std::string(rack.rack.view()) +
                        " declares an UNKNOWN failure domain class");
      }
      if (!hint.id.known() || !validate_identity(hint.id.view()).ok()) {
        return fail(PersistenceStatus::InvalidIdentity,
                    "rack " + std::string(rack.rack.view()) +
                        " has a malformed failure domain identity");
      }
      if (i > 0 && !(rack.failure_domain_hints[i - 1].klass < hint.klass) &&
          !(rack.failure_domain_hints[i - 1].klass == hint.klass &&
            rack.failure_domain_hints[i - 1].id < hint.id)) {
        return fail(PersistenceStatus::InvalidState,
                    "rack " + std::string(rack.rack.view()) +
                        " failure domain hints are duplicated or not sorted");
      }
    }
  }

  const auto rack_exists = [&rack_ids](const RackId& id) {
    return std::binary_search(rack_ids.begin(), rack_ids.end(), id);
  };
  const auto member_list_ok = [&rack_exists](const std::vector<RackId>& members,
                                             std::string& detail_out) {
    if (members.size() > kMaxDomainMembers) {
      detail_out = "member list exceeds the bound";
      return false;
    }
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (!members[i].known() || !validate_identity(members[i].view()).ok()) {
        detail_out = "member identity is missing or malformed";
        return false;
      }
      if (i > 0 && !(members[i - 1] < members[i])) {
        detail_out = "member list is duplicated or not sorted";
        return false;
      }
      if (!rack_exists(members[i])) {
        detail_out = "member rack " + std::string(members[i].view()) + " does not exist";
        return false;
      }
    }
    return true;
  };

  const auto check_domain_list = [&](const auto& domains, const char* label) -> PersistenceStatus {
    if (domains.size() > kMaxDomainsPerClass) {
      return fail(PersistenceStatus::AbsurdCount,
                  std::string(label) + " count exceeds the bound");
    }
    for (std::size_t i = 0; i < domains.size(); ++i) {
      const auto& domain = domains[i];
      if (!domain.id.known() || !validate_identity(domain.id.view()).ok()) {
        return fail(PersistenceStatus::InvalidIdentity,
                    std::string(label) + " identity is missing or malformed");
      }
      if (!domain.header.generation.known()) {
        return fail(PersistenceStatus::InvalidGeneration,
                    std::string(label) + " " + std::string(domain.id.view()) +
                        " has a zero generation");
      }
      if (i > 0 && !(domains[i - 1].id < domain.id)) {
        return fail(PersistenceStatus::InvalidState,
                    std::string(label) + " list is duplicated or not sorted");
      }
      std::string member_detail;
      if (!member_list_ok(domain.racks, member_detail)) {
        return fail(PersistenceStatus::DanglingReference,
                    std::string(label) + " " + std::string(domain.id.view()) + ": " +
                        member_detail);
      }
    }
    return PersistenceStatus::Ok;
  };

  if (const PersistenceStatus status =
          check_domain_list(state.placement_domains, "placement domain");
      status != PersistenceStatus::Ok) {
    return status;
  }
  if (const PersistenceStatus status =
          check_domain_list(state.capacity_domains, "capacity domain");
      status != PersistenceStatus::Ok) {
    return status;
  }
  if (const PersistenceStatus status =
          check_domain_list(state.failure_domains, "failure domain");
      status != PersistenceStatus::Ok) {
    return status;
  }
  if (const PersistenceStatus status = check_domain_list(state.network_domains, "network domain");
      status != PersistenceStatus::Ok) {
    return status;
  }
  if (const PersistenceStatus status = check_domain_list(state.storage_domains, "storage domain");
      status != PersistenceStatus::Ok) {
    return status;
  }
  if (const PersistenceStatus status = check_domain_list(state.power_domains, "power domain");
      status != PersistenceStatus::Ok) {
    return status;
  }
  if (const PersistenceStatus status = check_domain_list(state.cooling_domains, "cooling domain");
      status != PersistenceStatus::Ok) {
    return status;
  }
  if (const PersistenceStatus status = check_domain_list(state.link_domains, "link domain");
      status != PersistenceStatus::Ok) {
    return status;
  }

  for (const CapacityDomain& domain : state.capacity_domains) {
    if (domain.quantities.size() > kMaxCapacityQuantities) {
      return fail(PersistenceStatus::BoundsExceeded,
                  "capacity domain " + std::string(domain.id.view()) +
                      " declares too many quantities");
    }
    for (std::size_t i = 0; i < domain.quantities.size(); ++i) {
      const CapacityQuantity& quantity = domain.quantities[i];
      if (quantity.unit.empty() || quantity.unit.size() > kMaxEncodedStringBytes) {
        return fail(PersistenceStatus::InvalidState,
                    "capacity domain " + std::string(domain.id.view()) +
                        " has a quantity without a bounded unit");
      }
      if (quantity.value.has_value() && !std::isfinite(*quantity.value)) {
        return fail(PersistenceStatus::InvalidState,
                    "capacity domain " + std::string(domain.id.view()) +
                        " has a non-finite quantity");
      }
      if (i > 0 && !(domain.quantities[i - 1].unit < quantity.unit) &&
          !(domain.quantities[i - 1].unit == quantity.unit &&
            domain.quantities[i - 1].provenance < quantity.provenance)) {
        return fail(PersistenceStatus::InvalidState,
                    "capacity domain " + std::string(domain.id.view()) +
                        " quantities are duplicated or not sorted");
      }
    }
  }

  std::vector<NetworkDomainId> network_ids;
  network_ids.reserve(state.network_domains.size());
  for (const NetworkDomain& domain : state.network_domains) {
    network_ids.push_back(domain.id);
  }
  std::vector<LinkDomainId> link_ids;
  link_ids.reserve(state.link_domains.size());
  for (const LinkDomain& domain : state.link_domains) {
    link_ids.push_back(domain.id);
  }

  for (std::size_t i = 0; i < state.links.size(); ++i) {
    const InterRackLink& link = state.links[i];
    if (!link.id.known() || !validate_identity(link.id.view()).ok()) {
      return fail(PersistenceStatus::InvalidIdentity, "an inter-rack link identity is malformed");
    }
    if (!link.source.known() || !link.destination.known() || link.source == link.destination) {
      return fail(PersistenceStatus::InvalidState,
                  "link " + std::string(link.id.view()) +
                      " does not connect two distinct racks");
    }
    if (!rack_exists(link.source) || !rack_exists(link.destination)) {
      return fail(PersistenceStatus::DanglingReference,
                  "link " + std::string(link.id.view()) + " references an unknown rack");
    }
    if (!link.header.generation.known() || !link.topology_epoch.known()) {
      return fail(PersistenceStatus::InvalidGeneration,
                  "link " + std::string(link.id.view()) + " has a zero generation or epoch");
    }
    if (link.network_domain.has_value() &&
        !std::binary_search(network_ids.begin(), network_ids.end(), *link.network_domain)) {
      return fail(PersistenceStatus::DanglingReference,
                  "link " + std::string(link.id.view()) + " references an unknown network domain");
    }
    if (link.link_domain.has_value() &&
        !std::binary_search(link_ids.begin(), link_ids.end(), *link.link_domain)) {
      return fail(PersistenceStatus::DanglingReference,
                  "link " + std::string(link.id.view()) + " references an unknown link domain");
    }
    if (i > 0 && !(state.links[i - 1].id < link.id)) {
      return fail(PersistenceStatus::InvalidState,
                  "inter-rack links are duplicated or not sorted");
    }
  }

  for (std::size_t i = 0; i < state.constraints.size(); ++i) {
    const ClusterConstraint& constraint = state.constraints[i];
    if (!constraint.id.known() || !validate_identity(constraint.id.view()).ok()) {
      return fail(PersistenceStatus::InvalidIdentity, "a constraint identity is malformed");
    }
    if (constraint.kind == ConstraintKind::Unknown) {
      return fail(PersistenceStatus::InvalidEnum,
                  "constraint " + std::string(constraint.id.view()) + " has an UNKNOWN kind");
    }
    if (!constraint.header.generation.known()) {
      return fail(PersistenceStatus::InvalidGeneration,
                  "constraint " + std::string(constraint.id.view()) + " has a zero generation");
    }
    std::string member_detail;
    if (!member_list_ok(constraint.racks, member_detail)) {
      return fail(PersistenceStatus::DanglingReference,
                  "constraint " + std::string(constraint.id.view()) + ": " + member_detail);
    }
    if (i > 0 && !(state.constraints[i - 1].id < constraint.id)) {
      return fail(PersistenceStatus::InvalidState, "constraints are duplicated or not sorted");
    }
  }

  for (std::size_t i = 0; i < state.retired_racks.size(); ++i) {
    const RetiredIdentity& retired = state.retired_racks[i];
    if (!retired.rack.known() || !validate_identity(retired.rack.view()).ok()) {
      return fail(PersistenceStatus::InvalidIdentity, "a retired rack identity is malformed");
    }
    if (!retired.last_generation.known()) {
      return fail(PersistenceStatus::InvalidGeneration,
                  "retired rack " + std::string(retired.rack.view()) +
                      " has a zero last generation");
    }
    if (i > 0 && !(state.retired_racks[i - 1].rack < retired.rack)) {
      return fail(PersistenceStatus::InvalidState, "retired racks are duplicated or not sorted");
    }
  }
  for (std::size_t i = 0; i < state.withdrawn_racks.size(); ++i) {
    const RackId& rack = state.withdrawn_racks[i];
    if (!rack.known() || !validate_identity(rack.view()).ok()) {
      return fail(PersistenceStatus::InvalidIdentity, "a withdrawn rack identity is malformed");
    }
    if (i > 0 && !(state.withdrawn_racks[i - 1] < rack)) {
      return fail(PersistenceStatus::InvalidState, "withdrawn racks are duplicated or not sorted");
    }
    const bool retired = std::any_of(
        state.retired_racks.begin(), state.retired_racks.end(),
        [&rack](const RetiredIdentity& entry) { return entry.rack == rack; });
    if (retired) {
      return fail(PersistenceStatus::InvalidState,
                  "rack " + std::string(rack.view()) +
                      " is recorded as both withdrawn and retired");
    }
  }
  for (std::size_t i = 0; i < state.fenced_authorities.size(); ++i) {
    const FencedAuthority& fenced = state.fenced_authorities[i];
    if (!fenced.boot.known() || !validate_identity(fenced.boot.view()).ok()) {
      return fail(PersistenceStatus::InvalidIdentity, "a fenced boot identity is malformed");
    }
    if (!fenced.rack.known() || !validate_identity(fenced.rack.view()).ok()) {
      return fail(PersistenceStatus::InvalidIdentity, "a fenced rack identity is malformed");
    }
    if (fenced.reason.empty() || fenced.reason.size() > kMaxEncodedStringBytes) {
      return fail(PersistenceStatus::InvalidState,
                  "fenced boot " + std::string(fenced.boot.view()) + " has no bounded reason");
    }
  }

  if (detail != nullptr) {
    detail->clear();
  }
  return PersistenceStatus::Ok;
}

FilePersistenceStore::FilePersistenceStore(std::string path) : path_(std::move(path)) {}

FilePersistenceStore::~FilePersistenceStore() { discard_temporary(); }

void FilePersistenceStore::discard_temporary() noexcept {
  if (temporary_path_.empty()) {
    return;
  }
  std::remove(temporary_path_.c_str());
  temporary_path_.clear();
}

PersistenceStore::SaveOutcome FilePersistenceStore::save(const PersistedState& state) {
  SaveOutcome outcome;
  if (path_.empty()) {
    outcome.status = PersistenceStatus::IoError;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Persist,
                                          "empty_path", "no persistence path was configured");
    return outcome;
  }
  const EncodeOutcome encoded = encode_persisted_state(state);
  if (!encoded.ok()) {
    outcome.status = encoded.status;
    outcome.error = encoded.error;
    return outcome;
  }

  discard_temporary();
  ++temporary_counter_;
  temporary_path_ = path_ + ".tmp-" + std::to_string(process_identifier()) + "-" +
                    std::to_string(temporary_counter_);

  {
    std::ofstream stream(temporary_path_, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
      outcome.status = PersistenceStatus::TempFileUnavailable;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Persist,
                                            "temp_file_unavailable",
                                            "could not open the temporary container for writing");
      discard_temporary();
      return outcome;
    }
    stream.write(encoded.bytes.data(), static_cast<std::streamsize>(encoded.bytes.size()));
    stream.flush();
    if (!stream.good()) {
      outcome.status = PersistenceStatus::IoError;
      outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Persist,
                                            "write_failed", "temporary container write failed");
      stream.close();
      discard_temporary();
      return outcome;
    }
  }

#if defined(_WIN32)
  if (MoveFileExA(temporary_path_.c_str(), path_.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    outcome.status = PersistenceStatus::AtomicReplaceFailed;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Persist,
                                          "atomic_replace_failed",
                                          "could not atomically replace the committed container");
    discard_temporary();
    return outcome;
  }
#else
  if (std::rename(temporary_path_.c_str(), path_.c_str()) != 0) {
    outcome.status = PersistenceStatus::AtomicReplaceFailed;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Persist,
                                          "atomic_replace_failed",
                                          "could not atomically replace the committed container");
    discard_temporary();
    return outcome;
  }
#endif
  temporary_path_.clear();
  outcome.ok = true;
  outcome.status = PersistenceStatus::Ok;
  outcome.bytes_written = static_cast<std::uint64_t>(encoded.bytes.size());
  return outcome;
}

PersistenceStore::LoadOutcome FilePersistenceStore::load() {
  LoadOutcome outcome;
  if (path_.empty()) {
    outcome.status = PersistenceStatus::IoError;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Recover,
                                          "empty_path", "no persistence path was configured");
    return outcome;
  }
  std::ifstream stream(path_, std::ios::binary);
  if (!stream.is_open()) {
    outcome.status = PersistenceStatus::MissingFile;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Recover,
                                          "missing_file", "no durable container exists yet");
    return outcome;
  }
  std::string bytes;
  char buffer[64 * 1024];
  while (stream.good()) {
    stream.read(buffer, static_cast<std::streamsize>(sizeof(buffer)));
    const std::streamsize read = stream.gcount();
    if (read > 0) {
      if (bytes.size() + static_cast<std::size_t>(read) > kMaxPersistenceBytes) {
        outcome.status = PersistenceStatus::PayloadTooLarge;
        outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Recover,
                                              "payload_too_large",
                                              "durable container exceeds the size limit");
        return outcome;
      }
      bytes.append(buffer, static_cast<std::size_t>(read));
    }
  }
  if (!stream.eof()) {
    outcome.status = PersistenceStatus::IoError;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Recover,
                                          "read_failed", "durable container read failed");
    return outcome;
  }
  PersistedDecodeOutcome decoded = decode_persisted_state(bytes);
  outcome.status = decoded.status;
  outcome.error = decoded.error;
  outcome.state = std::move(decoded.state);
  return outcome;
}

PersistenceStore::SaveOutcome MemoryPersistenceStore::save(const PersistedState& state) {
  SaveOutcome outcome;
  const EncodeOutcome encoded = encode_persisted_state(state);
  if (!encoded.ok()) {
    outcome.status = encoded.status;
    outcome.error = encoded.error;
    return outcome;
  }
  bytes_ = encoded.bytes;
  ++save_count_;
  outcome.ok = true;
  outcome.status = PersistenceStatus::Ok;
  outcome.bytes_written = static_cast<std::uint64_t>(bytes_.size());
  return outcome;
}

PersistenceStore::LoadOutcome MemoryPersistenceStore::load() {
  LoadOutcome outcome;
  if (bytes_.empty()) {
    outcome.status = PersistenceStatus::MissingFile;
    outcome.error = StructuredError::make(ErrorCategory::Persistence, ErrorStage::Recover,
                                          "missing_file", "no durable container exists yet");
    return outcome;
  }
  PersistedDecodeOutcome decoded = decode_persisted_state(bytes_);
  outcome.status = decoded.status;
  outcome.error = decoded.error;
  outcome.state = std::move(decoded.state);
  return outcome;
}

void MemoryPersistenceStore::discard_temporary() noexcept {}

void MemoryPersistenceStore::set_raw_bytes(std::string bytes) { bytes_ = std::move(bytes); }

}  // namespace cluster_fabric
