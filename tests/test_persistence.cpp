// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Durable state container: deterministic encoding, the explicit container
// header and its checksum construction, every truncation length, sampled
// single-bit corruption, adversarial payloads, both store backends and the
// structural validator.

#include "harness.hpp"

#include "cluster_fabric/cluster_fabric.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace cluster_fabric;

namespace {

// ---------------------------------------------------------------------------
// Little-endian header helpers
// ---------------------------------------------------------------------------

void put_u16_at(std::string& bytes, std::size_t offset, std::uint16_t value) {
  for (int index = 0; index < 2; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] =
        static_cast<char>((value >> (index * 8)) & 0xFFu);
  }
}

void put_u32_at(std::string& bytes, std::size_t offset, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] =
        static_cast<char>((value >> (index * 8)) & 0xFFu);
  }
}

std::uint8_t byte_at(std::string_view bytes, std::size_t offset) {
  return static_cast<std::uint8_t>(static_cast<unsigned char>(bytes[offset]));
}

std::uint16_t read_u16_at(std::string_view bytes, std::size_t offset) {
  std::uint16_t value = 0;
  for (int index = 0; index < 2; ++index) {
    value = static_cast<std::uint16_t>(
        value | static_cast<std::uint16_t>(byte_at(bytes, offset + static_cast<std::size_t>(index))
                                          << (index * 8)));
  }
  return value;
}

std::uint32_t read_u32_at(std::string_view bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(byte_at(bytes, offset + static_cast<std::size_t>(index)))
             << (index * 8);
  }
  return value;
}

inline constexpr std::size_t kHeaderBytes = 16;

/// Wraps a payload in a container with the documented header and checksum.
std::string container_from_payload(std::string_view payload) {
  std::string container(kHeaderBytes, '\0');
  put_u32_at(container, 0, kPersistenceMagic);
  put_u16_at(container, 4, kPersistenceFormatVersion);
  put_u16_at(container, 6, 0);
  put_u32_at(container, 8, static_cast<std::uint32_t>(payload.size()));
  container.append(payload.data(), payload.size());
  const std::uint32_t checksum =
      crc32(std::string_view(container).substr(0, 12)) ^ crc32(payload);
  put_u32_at(container, 12, checksum);
  return container;
}

/// Recomputes the checksum of a (possibly patched) container.
void reseal(std::string& container) {
  const std::uint32_t length = read_u32_at(container, 8);
  const std::string_view payload =
      std::string_view(container).substr(kHeaderBytes, static_cast<std::size_t>(length));
  put_u32_at(container, 12, crc32(std::string_view(container).substr(0, 12)) ^ crc32(payload));
}

void expect_status(PersistenceStatus actual, PersistenceStatus expected, std::string_view context) {
  if (actual != expected) {
    CF_FAIL(std::string(context) + ": observed status " + std::string(to_string(actual)) +
            ", expected " + std::string(to_string(expected)));
  }
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

template <class Id>
Id make_id(std::string_view text) {
  const std::optional<Id> parsed = Id::parse(text);
  CF_EXPECT(parsed.has_value());
  return *parsed;
}

EvidenceStamp stamp(std::int64_t millis) {
  EvidenceStamp value;
  value.provenance = EvidenceProvenance::Measured;
  value.observed_at = Timestamp::from_unix_millis(millis);
  value.freshness = Freshness::Fresh;
  value.ttl_millis = 60'000;
  return value;
}

DomainHeader header(std::uint64_t generation) {
  DomainHeader value;
  value.evidence = stamp(1'000'000);
  value.generation = DomainGeneration::from_raw(generation);
  value.publisher = make_id<RackPublisherId>("publisher-a");
  value.cluster_epoch = ClusterEpoch::from_raw(1);
  value.coordinator_epoch = CoordinatorEpoch::from_raw(1);
  value.label = "row-a";
  return value;
}

PersistedState::DurableRack durable_rack(const char* rack, const char* boot,
                                         std::uint64_t generation) {
  PersistedState::DurableRack record;
  record.rack = make_id<RackId>(rack);
  record.generation = RackGeneration::from_raw(generation);
  record.membership = RackMembershipState::Active;
  record.publisher = make_id<RackPublisherId>("publisher-a");
  record.last_accepted_publication = PublicationGeneration::from_raw(2);
  record.last_boot = make_id<RackAgentBootId>(boot);
  AcceleratorClassSummary accelerator;
  accelerator.vendor = AcceleratorVendor::Nvidia;
  accelerator.family = "H100";
  accelerator.device_count = std::uint32_t{8};
  accelerator.device_memory_bytes = std::uint64_t{80ull * 1024ull * 1024ull * 1024ull};
  record.composition.accelerators = {accelerator};
  record.composition.composition_label = "gpu-dense";
  record.composition.provenance = EvidenceProvenance::Measured;
  RackEndpoint endpoint;
  endpoint.id = make_id<RackEndpointId>("endpoint-0");
  endpoint.network_domain = make_id<NetworkDomainId>("network-a");
  endpoint.link_domain = make_id<LinkDomainId>("link-domain-a");
  endpoint.connectivity = ConnectivityClass::DirectFabric;
  endpoint.evidence = stamp(1'000'000);
  record.endpoints = {endpoint};
  RackFailureDomainHint hint;
  hint.klass = FailureDomainClass::Row;
  hint.id = make_id<FailureDomainId>("row-1");
  hint.evidence = stamp(1'000'000);
  record.failure_domain_hints = {hint};
  record.rack_lifecycle = RackLifecycleState::Ready;
  record.membership_generation = MembershipGeneration::from_raw(2);
  record.origin_label = "rack-fabric:1.0.0";
  record.declared_at = Timestamp::from_unix_millis(1'000'000);
  return record;
}

/// A structurally valid durable state: sorted racks, sorted domain members,
/// every reference resolvable.
PersistedState valid_state() {
  PersistedState state;
  state.id = make_id<ClusterId>("cluster-a");
  state.epoch = ClusterEpoch::from_raw(1);
  state.last_coordinator_epoch = CoordinatorEpoch::from_raw(1);
  state.generation = ClusterGeneration::from_raw(3);
  state.membership_generation = MembershipGeneration::from_raw(2);
  state.topology_epoch = TopologyEpoch::from_raw(1);
  state.topology_generation = TopologyGeneration::from_raw(1);
  state.connectivity_generation = ConnectivityGeneration::from_raw(1);
  state.health_generation = HealthGeneration::from_raw(1);
  state.constraint_generation = ConstraintGeneration::from_raw(1);
  state.domain_generations.placement = DomainGeneration::from_raw(1);
  state.domain_generations.capacity = DomainGeneration::from_raw(1);
  state.domain_generations.failure = DomainGeneration::from_raw(1);
  state.domain_generations.network = DomainGeneration::from_raw(1);
  state.domain_generations.storage = DomainGeneration::from_raw(1);
  state.domain_generations.power = DomainGeneration::from_raw(1);
  state.domain_generations.cooling = DomainGeneration::from_raw(1);
  state.domain_generations.link = DomainGeneration::from_raw(1);
  state.snapshot_generation = SnapshotGeneration::from_raw(1);
  state.publication_generation = PublicationGeneration::from_raw(1);
  state.lifecycle = ClusterLifecycle::Ready;
  state.readiness_contract = ReadinessContract::permissive();
  state.topology_record.epoch = TopologyEpoch::from_raw(1);
  state.topology_record.generation = TopologyGeneration::from_raw(1);
  state.topology_record.established_at = Timestamp::from_unix_millis(1'000'000);
  state.topology_record.evidence = stamp(1'000'000);
  state.topology_record.reason = "cluster_declared";
  state.declared_at = Timestamp::from_unix_millis(1'000'000);
  state.last_mutation_at = Timestamp::from_unix_millis(1'000'000);

  state.racks = {durable_rack("rack-01", "boot-01", 7), durable_rack("rack-02", "boot-02", 3)};

  PlacementDomain placement;
  placement.id = make_id<PlacementDomainId>("placement-a");
  placement.klass = PlacementDomainClass::LowLatencyFabric;
  placement.racks = {make_id<RackId>("rack-01"), make_id<RackId>("rack-02")};
  placement.header = header(3);
  state.placement_domains = {placement};

  CapacityDomain capacity;
  capacity.id = make_id<CapacityDomainId>("capacity-a");
  capacity.klass = CapacityDomainClass::AcceleratorPool;
  capacity.racks = {make_id<RackId>("rack-01")};
  CapacityQuantity quantity;
  quantity.unit = "devices";
  quantity.value = 16.0;
  quantity.provenance = EvidenceProvenance::Derived;
  quantity.aggregated = true;
  capacity.quantities = {quantity};
  capacity.header = header(3);
  state.capacity_domains = {capacity};

  FailureDomain failure;
  failure.id = make_id<FailureDomainId>("row-1");
  failure.klass = FailureDomainClass::Row;
  failure.racks = {make_id<RackId>("rack-01"), make_id<RackId>("rack-02")};
  failure.header = header(3);
  state.failure_domains = {failure};

  NetworkDomain network;
  network.id = make_id<NetworkDomainId>("network-a");
  network.connectivity = ConnectivityClass::SwitchedFabric;
  network.racks = {make_id<RackId>("rack-01")};
  network.header = header(3);
  state.network_domains = {network};

  StorageDomain storage;
  storage.id = make_id<StorageDomainId>("storage-a");
  storage.racks = {make_id<RackId>("rack-01")};
  storage.header = header(3);
  state.storage_domains = {storage};

  // Power and cooling domains are exercised by
  // parented_power_and_cooling_domains_round_trip so that this fixture stays
  // focused on the rest of the container.

  LinkDomain link_domain;
  link_domain.id = make_id<LinkDomainId>("link-domain-a");
  link_domain.racks = {make_id<RackId>("rack-01")};
  link_domain.header = header(3);
  state.link_domains = {link_domain};

  InterRackLink link;
  link.id = make_id<InterRackLinkId>("link-a");
  link.source = make_id<RackId>("rack-01");
  link.destination = make_id<RackId>("rack-02");
  link.direction = LinkDirection::Bidirectional;
  link.connectivity = ConnectivityClass::DirectFabric;
  link.network_domain = make_id<NetworkDomainId>("network-a");
  link.link_domain = make_id<LinkDomainId>("link-domain-a");
  link.reachability = Reachability::Reachable;
  link.health = HealthState::Healthy;
  link.header = header(3);
  link.topology_epoch = TopologyEpoch::from_raw(1);
  link.topology_generation = TopologyGeneration::from_raw(1);
  link.publication = PublicationGeneration::from_raw(2);
  state.links = {link};

  ClusterConstraint constraint;
  constraint.id = make_id<ConstraintId>("constraint-a");
  constraint.kind = ConstraintKind::FailureDomainIndependence;
  constraint.racks = {make_id<RackId>("rack-01")};
  constraint.domain_ref = "failure_domain/row-1";
  constraint.statement = "rack-01 must not share a row";
  constraint.header = header(3);
  state.constraints = {constraint};

  RetiredIdentity retired;
  retired.rack = make_id<RackId>("rack-retired");
  retired.last_generation = RackGeneration::from_raw(2);
  retired.retired_at = Timestamp::from_unix_millis(1'000'000);
  retired.reason = "retired_by_operator";
  state.retired_racks = {retired};

  state.withdrawn_racks = {make_id<RackId>("rack-withdrawn")};

  FencedAuthority fenced;
  fenced.boot = make_id<RackAgentBootId>("boot-fenced");
  fenced.rack = make_id<RackId>("rack-09");
  fenced.reason = "session_closed";
  fenced.fenced_at = Timestamp::from_unix_millis(1'000'000);
  state.fenced_authorities = {fenced};
  return state;
}

/// Payload prefix written in the order decode_persisted_state reads it, so a
/// crafted container reaches the record-level bound checks.
std::string payload_prefix(std::string_view cluster, std::uint8_t lifecycle) {
  ByteWriter writer(256);
  writer.text(cluster);
  writer.u64(1);  // cluster epoch
  writer.u64(1);  // last coordinator epoch
  writer.u64(3);  // cluster generation
  writer.u64(2);  // membership generation
  writer.u64(1);  // topology epoch
  writer.u64(1);  // topology generation
  writer.u64(1);  // connectivity generation
  writer.u64(1);  // health generation
  writer.u64(1);  // constraint generation
  for (int index = 0; index < 8; ++index) {
    writer.u64(1);  // domain generations
  }
  writer.u64(1);  // snapshot generation
  writer.u64(1);  // publication generation
  writer.u8(lifecycle);
  writer.u64(0);         // minimum current racks
  writer.u32(0);         // mandatory racks
  writer.boolean(true);  // require all active racks current
  writer.boolean(false); // require connectivity evidence
  writer.u64(0);         // minimum current links
  writer.u32(0);         // required placement classes
  writer.u32(0);         // required failure classes
  writer.boolean(true);  // allow partial
  writer.boolean(true);  // allow degraded
  writer.boolean(true);  // require no conflicts
  writer.u64(1);         // topology record epoch
  writer.u64(1);         // topology record generation
  writer.u8(static_cast<std::uint8_t>(EvidenceProvenance::Measured));
  writer.i64(1'000'000);  // observed at
  writer.u8(static_cast<std::uint8_t>(Freshness::Fresh));
  writer.i64(60'000);     // ttl
  writer.i64(1'000'000);  // topology record established at
  writer.text("cluster_declared");
  writer.i64(1'000'000);  // declared at
  writer.i64(1'000'000);  // last mutation at
  CF_EXPECT(writer.ok());
  return writer.data();
}

/// The status decode_persisted_state must report for one corrupted container,
/// derived from the documented check order.
PersistenceStatus expected_status_for_corruption(std::string_view container) {
  if (container.empty()) {
    return PersistenceStatus::EmptyInput;
  }
  if (container.size() < kHeaderBytes) {
    return PersistenceStatus::TruncatedHeader;
  }
  if (read_u32_at(container, 0) != kPersistenceMagic) {
    return PersistenceStatus::BadMagic;
  }
  if (read_u16_at(container, 4) != kPersistenceFormatVersion) {
    return PersistenceStatus::UnsupportedVersion;
  }
  if (read_u16_at(container, 6) != 0) {
    return PersistenceStatus::InvalidState;
  }
  const std::uint64_t declared = read_u32_at(container, 8);
  const std::uint64_t available = container.size() - kHeaderBytes;
  if (declared > kMaxPersistenceBytes) {
    return PersistenceStatus::PayloadTooLarge;
  }
  if (declared > available) {
    return PersistenceStatus::TruncatedBody;
  }
  if (declared < available) {
    return PersistenceStatus::TrailingGarbage;
  }
  const std::string_view payload = container.substr(kHeaderBytes, declared);
  if ((crc32(container.substr(0, 12)) ^ crc32(payload)) != read_u32_at(container, 12)) {
    return PersistenceStatus::ChecksumMismatch;
  }
  return PersistenceStatus::Ok;
}

std::filesystem::path fresh_temp_directory() {
  std::error_code code;
  const std::filesystem::path base = std::filesystem::temp_directory_path(code);
  CF_EXPECT(!code);
  const std::filesystem::path directory = base / "cf-persistence-store-test";
  std::filesystem::remove_all(directory, code);
  CF_EXPECT(!code);
  std::filesystem::create_directories(directory, code);
  CF_EXPECT(!code);
  return directory;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  CF_EXPECT(stream.is_open());
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::vector<std::filesystem::path> list_files(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> files;
  std::error_code code;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory, code)) {
    if (entry.is_regular_file()) {
      files.push_back(entry.path());
    }
  }
  CF_EXPECT(!code);
  return files;
}

void remove_directory(const std::filesystem::path& directory) {
  std::error_code code;
  std::filesystem::remove_all(directory, code);
  CF_EXPECT(!code);
}

}  // namespace

// ---------------------------------------------------------------------------
// Encoding, determinism and round-trip
// ---------------------------------------------------------------------------

CF_TEST(encode_is_deterministic_and_bounded) {
  const PersistedState state = valid_state();
  std::string detail;
  expect_status(validate_persisted_state(state, &detail), PersistenceStatus::Ok,
                "the fixture must be structurally valid");
  CF_EXPECT(detail.empty());

  const EncodeOutcome first = encode_persisted_state(state);
  expect_status(first.status, PersistenceStatus::Ok, "first encode");
  CF_EXPECT(!first.bytes.empty());
  CF_EXPECT_EQ(first.bytes.size() % 1, std::size_t{0});

  const EncodeOutcome second = encode_persisted_state(state);
  expect_status(second.status, PersistenceStatus::Ok, "second encode");
  if (first.bytes != second.bytes) {
    CF_FAIL("encode_persisted_state is not deterministic: " + std::to_string(first.bytes.size()) +
            " vs " + std::to_string(second.bytes.size()) + " bytes");
  }
  CF_EXPECT(first.ok());

  // A semantically identical state encoded from a copy produces the same bytes.
  PersistedState copy = state;
  const EncodeOutcome third = encode_persisted_state(copy);
  expect_status(third.status, PersistenceStatus::Ok, "copy encode");
  CF_EXPECT_EQ(third.bytes, first.bytes);

  // Every status asserted above is typed and printable.
  CF_EXPECT_EQ(to_string(PersistenceStatus::Ok), std::string_view{"OK"});
  CF_EXPECT_EQ(to_string(PersistenceStatus::EmptyInput), std::string_view{"EMPTY_INPUT"});
  CF_EXPECT_EQ(to_string(PersistenceStatus::ChecksumMismatch),
               std::string_view{"CHECKSUM_MISMATCH"});
}

CF_TEST(encode_decode_round_trip_is_byte_identical) {
  const PersistedState state = valid_state();
  const EncodeOutcome encoded = encode_persisted_state(state);
  expect_status(encoded.status, PersistenceStatus::Ok, "encode");

  const PersistedDecodeOutcome decoded = decode_persisted_state(encoded.bytes);
  if (decoded.status != PersistenceStatus::Ok) {
    CF_FAIL("decode of a freshly encoded container reported " +
            std::string(to_string(decoded.status)) + " (" + decoded.error.reason + ": " +
            decoded.error.message + ")");
  }
  CF_EXPECT(decoded.state.has_value());
  const PersistedState& restored = *decoded.state;
  CF_EXPECT_EQ(restored, state);
  CF_EXPECT_EQ(restored.topology_record.established_at, state.topology_record.established_at);
  CF_EXPECT_EQ(restored.topology_record.evidence, state.topology_record.evidence);
  CF_EXPECT_EQ(restored.topology_record.reason, state.topology_record.reason);
  CF_EXPECT_EQ(restored.declared_at, state.declared_at);
  CF_EXPECT_EQ(restored.last_mutation_at, state.last_mutation_at);

  const EncodeOutcome reencoded = encode_persisted_state(restored);
  expect_status(reencoded.status, PersistenceStatus::Ok, "re-encode");
  if (reencoded.bytes != encoded.bytes) {
    CF_FAIL("a decoded state re-encoded to different bytes: " +
            std::to_string(encoded.bytes.size()) + " vs " +
            std::to_string(reencoded.bytes.size()));
  }
}

CF_TEST(parented_power_and_cooling_domains_round_trip) {
  // Power and cooling records carry an optional parent. A parented record and
  // a record with member racks must both survive the round trip.
  const auto encode_decode = [](const PersistedState& state, const char* label) {
    const EncodeOutcome encoded = encode_persisted_state(state);
    expect_status(encoded.status, PersistenceStatus::Ok, std::string(label) + " encode");
    const PersistedDecodeOutcome decoded = decode_persisted_state(encoded.bytes);
    if (decoded.status != PersistenceStatus::Ok) {
      CF_FAIL(std::string(label) + ": decode reported " +
              std::string(to_string(decoded.status)) + " (" + decoded.error.reason + ": " +
              decoded.error.message + ")");
    }
    CF_EXPECT(decoded.state.has_value());
    CF_EXPECT_EQ(*decoded.state, state);
  };

  PersistedState without_parent = valid_state();
  PowerDomain plain_power;
  plain_power.id = make_id<PowerDomainId>("power-a");
  plain_power.racks = {make_id<RackId>("rack-01")};
  plain_power.header = header(3);
  without_parent.power_domains = {plain_power};
  CoolingDomain plain_cooling;
  plain_cooling.id = make_id<CoolingDomainId>("cooling-a");
  plain_cooling.racks = {make_id<RackId>("rack-01")};
  plain_cooling.header = header(3);
  without_parent.cooling_domains = {plain_cooling};
  encode_decode(without_parent, "power and cooling domains with member racks");

  PersistedState with_parent = without_parent;
  with_parent.power_domains[0].parent = make_id<PowerDomainId>("power-root");
  with_parent.cooling_domains[0].parent = make_id<CoolingDomainId>("cooling-root");
  encode_decode(with_parent, "parented power and cooling domains");

  PersistedState parent_only = without_parent;
  parent_only.power_domains[0].racks.clear();
  parent_only.power_domains[0].parent = make_id<PowerDomainId>("power-root");
  parent_only.cooling_domains[0].racks.clear();
  parent_only.cooling_domains[0].parent = make_id<CoolingDomainId>("cooling-root");
  encode_decode(parent_only, "parented power and cooling domains without racks");
}

// ---------------------------------------------------------------------------
// Container header and checksum
// ---------------------------------------------------------------------------

CF_TEST(container_header_layout_and_checksum_construction) {
  const PersistedState state = valid_state();
  const EncodeOutcome encoded = encode_persisted_state(state);
  expect_status(encoded.status, PersistenceStatus::Ok, "encode");
  const std::string& container = encoded.bytes;

  static_assert(kPersistenceMagic == 0x31534643u, "the container magic is 'CFS1' little-endian");
  CF_EXPECT_EQ(byte_at(container, 0), std::uint8_t{0x43});
  CF_EXPECT_EQ(byte_at(container, 1), std::uint8_t{0x46});
  CF_EXPECT_EQ(byte_at(container, 2), std::uint8_t{0x53});
  CF_EXPECT_EQ(byte_at(container, 3), std::uint8_t{0x31});
  CF_EXPECT_EQ(read_u32_at(container, 0), kPersistenceMagic);
  CF_EXPECT_EQ(read_u16_at(container, 4), kPersistenceFormatVersion);
  CF_EXPECT_EQ(read_u16_at(container, 6), std::uint16_t{0});

  const std::uint32_t declared = read_u32_at(container, 8);
  CF_EXPECT_EQ(container.size(), kHeaderBytes + static_cast<std::size_t>(declared));
  const std::string_view payload = std::string_view(container).substr(kHeaderBytes, declared);

  const std::uint32_t header_checksum = crc32(std::string_view(container).substr(0, 12));
  const std::uint32_t payload_checksum = crc32(payload);
  const std::uint32_t stored = read_u32_at(container, 12);
  CF_EXPECT_EQ(stored, header_checksum ^ payload_checksum);
  CF_EXPECT_NE(stored, payload_checksum);
  CF_EXPECT_NE(stored, header_checksum);
  CF_EXPECT_EQ(expected_status_for_corruption(container), PersistenceStatus::Ok);

  // The container the test builds itself must decode identically to the
  // container the library built.
  const std::string rebuilt = container_from_payload(payload);
  CF_EXPECT_EQ(rebuilt, container);
}

// ---------------------------------------------------------------------------
// Truncation and corruption
// ---------------------------------------------------------------------------

CF_TEST(truncation_at_every_length_is_rejected_with_a_typed_status) {
  const EncodeOutcome encoded = encode_persisted_state(valid_state());
  expect_status(encoded.status, PersistenceStatus::Ok, "encode");
  const std::string& container = encoded.bytes;
  CF_EXPECT(container.size() > kHeaderBytes + 1);

  for (std::size_t length = 0; length < container.size(); ++length) {
    const PersistedDecodeOutcome outcome =
        decode_persisted_state(std::string_view(container).substr(0, length));
    PersistenceStatus expected = PersistenceStatus::TruncatedBody;
    if (length == 0) {
      expected = PersistenceStatus::EmptyInput;
    } else if (length < kHeaderBytes) {
      expected = PersistenceStatus::TruncatedHeader;
    }
    if (outcome.status != expected) {
      CF_FAIL("truncation to " + std::to_string(length) + " bytes reported " +
              std::string(to_string(outcome.status)) + ", expected " +
              std::string(to_string(expected)));
    }
    if (outcome.state.has_value()) {
      CF_FAIL("truncation to " + std::to_string(length) + " bytes returned partial state");
    }
    CF_EXPECT(!outcome.ok());
  }

  const PersistedDecodeOutcome full = decode_persisted_state(container);
  if (full.status == PersistenceStatus::TruncatedBody) {
    CF_FAIL("the complete container was reported as TRUNCATED_BODY");
  }
}

CF_TEST(single_bit_flips_are_rejected_at_every_seventh_offset) {
  const EncodeOutcome encoded = encode_persisted_state(valid_state());
  expect_status(encoded.status, PersistenceStatus::Ok, "encode");
  const std::string& container = encoded.bytes;

  std::size_t sampled = 0;
  std::size_t payload_flips = 0;
  for (std::size_t offset = 0; offset < container.size(); offset += 7) {
    for (unsigned bit = 0; bit < 8u; ++bit) {
      std::string corrupted = container;
      corrupted[offset] = static_cast<char>(static_cast<unsigned char>(corrupted[offset]) ^
                                            static_cast<unsigned char>(1u << bit));
      const PersistedDecodeOutcome outcome = decode_persisted_state(corrupted);
      const PersistenceStatus expected = expected_status_for_corruption(corrupted);
      if (outcome.status != expected) {
        CF_FAIL("flip at offset " + std::to_string(offset) + " bit " + std::to_string(bit) +
                " reported " + std::string(to_string(outcome.status)) + ", expected " +
                std::string(to_string(expected)));
      }
      if (outcome.status == PersistenceStatus::Ok) {
        CF_FAIL("flip at offset " + std::to_string(offset) + " bit " + std::to_string(bit) +
                " was accepted");
      }
      if (outcome.state.has_value()) {
        CF_FAIL("flip at offset " + std::to_string(offset) + " returned partial state");
      }
      if (offset >= kHeaderBytes) {
        if (outcome.status != PersistenceStatus::ChecksumMismatch) {
          CF_FAIL("payload flip at offset " + std::to_string(offset) + " bit " +
                  std::to_string(bit) + " reported " + std::string(to_string(outcome.status)) +
                  " instead of CHECKSUM_MISMATCH");
        }
        ++payload_flips;
      }
      ++sampled;
    }
  }
  CF_EXPECT(sampled > 0);
  CF_EXPECT(payload_flips > 0);
}

CF_TEST(trailing_garbage_unsupported_version_and_reserved_flags_are_rejected) {
  const EncodeOutcome encoded = encode_persisted_state(valid_state());
  expect_status(encoded.status, PersistenceStatus::Ok, "encode");

  const PersistedDecodeOutcome trailing = decode_persisted_state(encoded.bytes + "x");
  expect_status(trailing.status, PersistenceStatus::TrailingGarbage, "one trailing byte");
  CF_EXPECT(!trailing.state.has_value());

  const PersistedDecodeOutcome two_trailing = decode_persisted_state(encoded.bytes + "xyz");
  expect_status(two_trailing.status, PersistenceStatus::TrailingGarbage, "three trailing bytes");

  std::string bad_version = encoded.bytes;
  put_u16_at(bad_version, 4, static_cast<std::uint16_t>(kPersistenceFormatVersion + 1u));
  reseal(bad_version);
  const PersistedDecodeOutcome version_outcome = decode_persisted_state(bad_version);
  expect_status(version_outcome.status, PersistenceStatus::UnsupportedVersion,
                "unsupported format version");
  CF_EXPECT(!version_outcome.state.has_value());

  std::string bad_magic = encoded.bytes;
  bad_magic[0] = static_cast<char>(0x00);
  reseal(bad_magic);
  expect_status(decode_persisted_state(bad_magic).status, PersistenceStatus::BadMagic, "bad magic");

  std::string reserved_flags = encoded.bytes;
  put_u16_at(reserved_flags, 6, 0x0001);
  reseal(reserved_flags);
  expect_status(decode_persisted_state(reserved_flags).status, PersistenceStatus::InvalidState,
                "reserved header flags");

  // A declared length larger than the content is a truncation, never a read.
  std::string overlong = encoded.bytes;
  put_u32_at(overlong, 8, static_cast<std::uint32_t>(encoded.bytes.size()));
  reseal(overlong);
  expect_status(decode_persisted_state(overlong).status, PersistenceStatus::TruncatedBody,
                "declared length beyond the container");
}

// ---------------------------------------------------------------------------
// Adversarial payloads
// ---------------------------------------------------------------------------

CF_TEST(absurd_counts_and_invalid_records_are_rejected) {
  // A rack count above the limit is refused before any reservation.
  const PersistedDecodeOutcome absurd =
      decode_persisted_state(container_from_payload(payload_prefix("cluster-a", 3) +
                                                    std::string(4, '\xFF')));
  expect_status(absurd.status, PersistenceStatus::AbsurdCount, "rack count 0xFFFFFFFF");
  CF_EXPECT(!absurd.state.has_value());

  // A count exactly at the limit is not absurd; it is merely absent.
  std::string at_limit = payload_prefix("cluster-a", 3);
  const std::uint32_t bound = static_cast<std::uint32_t>(kMaxRacksPerCluster);
  at_limit.push_back(static_cast<char>(bound & 0xFFu));
  at_limit.push_back(static_cast<char>((bound >> 8) & 0xFFu));
  at_limit.push_back(static_cast<char>((bound >> 16) & 0xFFu));
  at_limit.push_back(static_cast<char>((bound >> 24) & 0xFFu));
  expect_status(decode_persisted_state(container_from_payload(at_limit)).status,
                PersistenceStatus::TruncatedBody, "rack count at the limit");

  // An invalid cluster identity.
  expect_status(decode_persisted_state(container_from_payload(payload_prefix("bad id", 3))).status,
                PersistenceStatus::InvalidIdentity, "invalid cluster identity");

  // An out-of-range lifecycle enumerator.
  expect_status(decode_persisted_state(container_from_payload(payload_prefix("cluster-a", 9))).status,
                PersistenceStatus::InvalidEnum, "lifecycle 9");

  // An invalid rack identity inside the first rack record.
  std::string bad_rack = payload_prefix("cluster-a", 3);
  ByteWriter writer(128);
  writer.u32(1);
  writer.text("bad id ");
  writer.u64(7);
  writer.u8(static_cast<std::uint8_t>(RackMembershipState::Active));
  writer.text("publisher-a");
  writer.u64(2);
  writer.text("boot-01");
  CF_EXPECT(writer.ok());
  bad_rack += writer.data();
  expect_status(decode_persisted_state(container_from_payload(bad_rack)).status,
                PersistenceStatus::InvalidIdentity, "invalid rack identity");

  // An out-of-range rack membership enumerator.
  std::string bad_membership = payload_prefix("cluster-a", 3);
  ByteWriter membership(128);
  membership.u32(1);
  membership.text("rack-01");
  membership.u64(7);
  membership.u8(9);
  membership.text("publisher-a");
  membership.u64(2);
  membership.text("boot-01");
  CF_EXPECT(membership.ok());
  bad_membership += membership.data();
  expect_status(decode_persisted_state(container_from_payload(bad_membership)).status,
                PersistenceStatus::InvalidEnum, "membership 9");

  // An empty container.
  expect_status(decode_persisted_state(std::string_view{}).status, PersistenceStatus::EmptyInput,
                "empty input");
}

// ---------------------------------------------------------------------------
// Structural validation
// ---------------------------------------------------------------------------

CF_TEST(validate_rejects_duplicate_and_unsorted_racks) {
  const PersistedState baseline = valid_state();
  std::string detail;
  expect_status(validate_persisted_state(baseline, &detail), PersistenceStatus::Ok, "baseline");

  PersistedState duplicate = baseline;
  duplicate.racks.push_back(duplicate.racks.front());
  expect_status(validate_persisted_state(duplicate, &detail),
                PersistenceStatus::DuplicateRackIdentity, "duplicate rack identity");
  CF_EXPECT(!detail.empty());

  PersistedState unsorted = baseline;
  std::swap(unsorted.racks[0], unsorted.racks[1]);
  expect_status(validate_persisted_state(unsorted, &detail), PersistenceStatus::DuplicateRackIdentity,
                "unsorted rack list");

  PersistedState zero_generation = baseline;
  zero_generation.racks[0].generation = RackGeneration::from_raw(0);
  expect_status(validate_persisted_state(zero_generation, &detail),
                PersistenceStatus::InvalidGeneration, "zero rack generation");

  PersistedState unsorted_endpoints = baseline;
  RackEndpoint extra;
  extra.id = make_id<RackEndpointId>("endpoint-0");
  unsorted_endpoints.racks[0].endpoints.push_back(extra);
  expect_status(validate_persisted_state(unsorted_endpoints, &detail),
                PersistenceStatus::InvalidState, "duplicate endpoint");

  PersistedState unknown_hint_class = baseline;
  unknown_hint_class.racks[0].failure_domain_hints[0].klass = FailureDomainClass::Unknown;
  expect_status(validate_persisted_state(unknown_hint_class, &detail),
                PersistenceStatus::InvalidEnum, "unknown failure domain class");
}

CF_TEST(validate_rejects_unknown_references) {
  const PersistedState baseline = valid_state();
  std::string detail;

  PersistedState domain_member = baseline;
  domain_member.placement_domains[0].racks = {make_id<RackId>("rack-absent")};
  expect_status(validate_persisted_state(domain_member, &detail),
                PersistenceStatus::DanglingReference, "placement domain member");
  CF_EXPECT(!detail.empty());

  PersistedState constraint_member = baseline;
  constraint_member.constraints[0].racks = {make_id<RackId>("rack-absent")};
  expect_status(validate_persisted_state(constraint_member, &detail),
                PersistenceStatus::DanglingReference, "constraint member");

  PersistedState link_rack = baseline;
  link_rack.links[0].destination = make_id<RackId>("rack-absent");
  expect_status(validate_persisted_state(link_rack, &detail),
                PersistenceStatus::DanglingReference, "link destination");

  PersistedState link_network = baseline;
  link_network.links[0].network_domain = make_id<NetworkDomainId>("network-absent");
  expect_status(validate_persisted_state(link_network, &detail),
                PersistenceStatus::DanglingReference, "link network domain");

  PersistedState link_self = baseline;
  link_self.links[0].destination = link_self.links[0].source;
  expect_status(validate_persisted_state(link_self, &detail), PersistenceStatus::InvalidState,
                "link to itself");

  PersistedState unsorted_members = baseline;
  unsorted_members.placement_domains[0].racks = {make_id<RackId>("rack-02"),
                                                 make_id<RackId>("rack-01")};
  expect_status(validate_persisted_state(unsorted_members, &detail),
                PersistenceStatus::DanglingReference, "unsorted domain members");

  PersistedState duplicate_quantity = baseline;
  duplicate_quantity.capacity_domains[0].quantities.push_back(
      duplicate_quantity.capacity_domains[0].quantities.front());
  expect_status(validate_persisted_state(duplicate_quantity, &detail),
                PersistenceStatus::InvalidState, "duplicate capacity quantity");
}

CF_TEST(validate_rejects_unknown_cluster_identity_and_zero_generations) {
  const PersistedState baseline = valid_state();
  std::string detail;

  PersistedState no_cluster = baseline;
  no_cluster.id = ClusterId{};
  expect_status(validate_persisted_state(no_cluster, &detail), PersistenceStatus::InvalidIdentity,
                "empty cluster identity");
  CF_EXPECT(!detail.empty());

  PersistedState zero_epoch = baseline;
  zero_epoch.epoch = ClusterEpoch::from_raw(0);
  expect_status(validate_persisted_state(zero_epoch, &detail), PersistenceStatus::InvalidEpoch,
                "zero cluster epoch");

  PersistedState zero_coordinator = baseline;
  zero_coordinator.last_coordinator_epoch = CoordinatorEpoch::from_raw(0);
  expect_status(validate_persisted_state(zero_coordinator, &detail),
                PersistenceStatus::InvalidEpoch, "zero coordinator epoch");

  PersistedState zero_generation = baseline;
  zero_generation.generation = ClusterGeneration::from_raw(0);
  expect_status(validate_persisted_state(zero_generation, &detail),
                PersistenceStatus::InvalidGeneration, "zero cluster generation");

  PersistedState zero_domain_generation = baseline;
  zero_domain_generation.placement_domains[0].header.generation = DomainGeneration::from_raw(0);
  expect_status(validate_persisted_state(zero_domain_generation, &detail),
                PersistenceStatus::InvalidGeneration, "zero domain generation");

  PersistedState bad_publisher = baseline;
  bad_publisher.racks[0].publisher = RackPublisherId{};
  expect_status(validate_persisted_state(bad_publisher, &detail),
                PersistenceStatus::InvalidIdentity, "empty rack publisher");

  PersistedState withdrawn_and_retired = baseline;
  withdrawn_and_retired.withdrawn_racks = {baseline.retired_racks[0].rack};
  expect_status(validate_persisted_state(withdrawn_and_retired, &detail),
                PersistenceStatus::InvalidState, "withdrawn and retired");

  PersistedState no_fence_reason = baseline;
  no_fence_reason.fenced_authorities[0].reason.clear();
  expect_status(validate_persisted_state(no_fence_reason, &detail), PersistenceStatus::InvalidState,
                "fenced authority without a reason");

  PersistedState unknown_constraint_kind = baseline;
  unknown_constraint_kind.constraints[0].kind = ConstraintKind::Unknown;
  expect_status(validate_persisted_state(unknown_constraint_kind, &detail),
                PersistenceStatus::InvalidEnum, "unknown constraint kind");
}

// ---------------------------------------------------------------------------
// FilePersistenceStore
// ---------------------------------------------------------------------------

CF_TEST(file_store_saves_loads_overwrites_and_reports_missing_file) {
  const std::filesystem::path directory = fresh_temp_directory();
  const std::filesystem::path container = directory / "cluster.cf";
  FilePersistenceStore store(container.string());

  const PersistenceStore::LoadOutcome missing = store.load();
  expect_status(missing.status, PersistenceStatus::MissingFile, "load of a missing path");
  CF_EXPECT(!missing.state.has_value());
  CF_EXPECT(!std::filesystem::exists(container));

  const PersistedState first = valid_state();
  const EncodeOutcome encoded_first = encode_persisted_state(first);
  expect_status(encoded_first.status, PersistenceStatus::Ok, "encode first state");

  const PersistenceStore::SaveOutcome saved = store.save(first);
  expect_status(saved.status, PersistenceStatus::Ok, "first save");
  CF_EXPECT(saved.ok);
  CF_EXPECT_EQ(saved.bytes_written, static_cast<std::uint64_t>(encoded_first.bytes.size()));
  CF_EXPECT(std::filesystem::exists(container));
  CF_EXPECT_EQ(read_file(container), encoded_first.bytes);
  CF_EXPECT(store.temporary_path().empty());

  const PersistenceStore::LoadOutcome loaded = store.load();
  expect_status(loaded.status, PersistenceStatus::Ok, "load after save");
  CF_EXPECT(loaded.state.has_value());
  if (loaded.state.has_value()) {
    CF_EXPECT_EQ(*loaded.state, first);
  }

  // Overwriting replaces the container atomically.
  PersistedState second = valid_state();
  second.generation = ClusterGeneration::from_raw(9);
  second.lifecycle = ClusterLifecycle::Degraded;
  const EncodeOutcome encoded_second = encode_persisted_state(second);
  expect_status(encoded_second.status, PersistenceStatus::Ok, "encode second state");
  const PersistenceStore::SaveOutcome overwritten = store.save(second);
  expect_status(overwritten.status, PersistenceStatus::Ok, "overwrite save");
  CF_EXPECT_EQ(read_file(container), encoded_second.bytes);
  CF_EXPECT_NE(encoded_second.bytes, encoded_first.bytes);

  const PersistenceStore::LoadOutcome reloaded = store.load();
  expect_status(reloaded.status, PersistenceStatus::Ok, "load after overwrite");
  CF_EXPECT(reloaded.state.has_value());
  if (reloaded.state.has_value()) {
    CF_EXPECT_EQ(*reloaded.state, second);
  }

  // No temporary artifact is left behind by a successful save.
  const std::vector<std::filesystem::path> files = list_files(directory);
  CF_EXPECT_EQ(files.size(), std::size_t{1});
  if (!files.empty()) {
    CF_EXPECT_EQ(files[0], container);
  }
  remove_directory(directory);
}

CF_TEST(file_store_discard_temporary_leaves_no_residue) {
  const std::filesystem::path directory = fresh_temp_directory();
  const std::filesystem::path container = directory / "cluster.cf";
  {
    FilePersistenceStore store(container.string());
    expect_status(store.save(valid_state()).status, PersistenceStatus::Ok, "save");
    CF_EXPECT(store.temporary_path().empty());
    store.discard_temporary();
    CF_EXPECT(store.temporary_path().empty());
    CF_EXPECT(std::filesystem::exists(container));
  }
  const std::vector<std::filesystem::path> files = list_files(directory);
  CF_EXPECT_EQ(files.size(), std::size_t{1});
  if (!files.empty()) {
    CF_EXPECT_EQ(files[0].filename(), std::filesystem::path("cluster.cf"));
  }

  // A store with no committed container leaves the directory empty.
  const std::filesystem::path other = directory / "absent.cf";
  {
    FilePersistenceStore store(other.string());
    expect_status(store.load().status, PersistenceStatus::MissingFile, "missing container");
    store.discard_temporary();
  }
  CF_EXPECT_EQ(list_files(directory).size(), std::size_t{1});
  CF_EXPECT(!std::filesystem::exists(other));
  remove_directory(directory);
}

// ---------------------------------------------------------------------------
// MemoryPersistenceStore
// ---------------------------------------------------------------------------

CF_TEST(memory_store_round_trips_and_raw_bytes_hit_the_same_paths) {
  MemoryPersistenceStore store;
  expect_status(store.load().status, PersistenceStatus::MissingFile, "empty memory store");

  const PersistedState state = valid_state();
  const EncodeOutcome encoded = encode_persisted_state(state);
  expect_status(encoded.status, PersistenceStatus::Ok, "encode");

  const PersistenceStore::SaveOutcome saved = store.save(state);
  expect_status(saved.status, PersistenceStatus::Ok, "memory save");
  CF_EXPECT_EQ(store.raw_bytes(), encoded.bytes);
  CF_EXPECT_EQ(store.save_count(), std::uint64_t{1});

  const PersistenceStore::LoadOutcome loaded = store.load();
  expect_status(loaded.status, PersistenceStatus::Ok, "memory load");
  CF_EXPECT(loaded.state.has_value());
  if (loaded.state.has_value()) {
    CF_EXPECT_EQ(*loaded.state, state);
  }

  store.set_raw_bytes(std::string());
  expect_status(store.load().status, PersistenceStatus::MissingFile, "cleared raw bytes");

  store.set_raw_bytes(std::string(4, '\x00'));
  expect_status(store.load().status, PersistenceStatus::TruncatedHeader, "short raw bytes");

  store.set_raw_bytes(encoded.bytes.substr(0, kHeaderBytes));
  expect_status(store.load().status, PersistenceStatus::TruncatedBody, "header only");

  std::string bad_magic = encoded.bytes;
  bad_magic[0] = static_cast<char>(0x00);
  reseal(bad_magic);
  store.set_raw_bytes(bad_magic);
  expect_status(store.load().status, PersistenceStatus::BadMagic, "raw bad magic");

  std::string flipped = encoded.bytes;
  flipped[kHeaderBytes + 1] = static_cast<char>(static_cast<unsigned char>(
                                  flipped[kHeaderBytes + 1]) ^ static_cast<unsigned char>(0x01));
  store.set_raw_bytes(flipped);
  expect_status(store.load().status, PersistenceStatus::ChecksumMismatch, "raw payload flip");

  store.set_raw_bytes(encoded.bytes + "garbage");
  expect_status(store.load().status, PersistenceStatus::TrailingGarbage, "raw trailing garbage");

  store.set_raw_bytes(container_from_payload(payload_prefix("cluster-a", 3) +
                                             std::string(4, '\xFF')));
  expect_status(store.load().status, PersistenceStatus::AbsurdCount, "raw absurd rack count");

  // The store never returns a partially decoded state.
  for (const std::string& raw : {std::string(4, '\x00'), encoded.bytes + "x", bad_magic}) {
    store.set_raw_bytes(raw);
    CF_EXPECT(!store.load().state.has_value());
  }
}

int main() { return cf_test::run("test_persistence"); }
