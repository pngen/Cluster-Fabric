// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Versioned, integrity-checked persistence.
//
// The container is explicit: magic, format version, declared payload length, a
// CRC32 over header and payload, then a deterministic binary body. Decoding
// produces a candidate state that is fully validated before it is committed.
// Corruption, truncation, trailing garbage, absurd counts, invalid enums and
// dangling references are all detected and reported as typed statuses.
//
// Durable state is separated from dynamic observation. Live process authority,
// reachability, health and currentness are never restored as current: they are
// cleared and require revalidation.

#ifndef CLUSTER_FABRIC_PERSISTENCE_HPP
#define CLUSTER_FABRIC_PERSISTENCE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cluster_fabric/cluster_state.hpp"
#include "cluster_fabric/error.hpp"
#include "cluster_fabric/generation.hpp"
#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/lifecycle.hpp"
#include "cluster_fabric/rack_reference.hpp"

namespace cluster_fabric {

enum class PersistenceStatus : std::uint8_t {
  Ok = 0,
  EmptyInput,
  MissingFile,
  BadMagic,
  UnsupportedVersion,
  TruncatedHeader,
  TruncatedBody,
  ChecksumMismatch,
  TrailingGarbage,
  InvalidEnum,
  AbsurdCount,
  BoundsExceeded,
  DuplicateRackIdentity,
  DanglingReference,
  InvalidEpoch,
  InvalidGeneration,
  InvalidIdentity,
  InvalidState,
  PayloadTooLarge,
  IoError,
  AtomicReplaceFailed,
  TempFileUnavailable,
};

[[nodiscard]] std::string_view to_string(PersistenceStatus value) noexcept;

/// The durable subset of canonical state. Everything a coordinator must
/// remember across a restart, and nothing it must not trust.
struct PersistedState {
  ClusterId id;
  ClusterEpoch epoch;
  CoordinatorEpoch last_coordinator_epoch;
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
  Timestamp declared_at = Timestamp::unknown();
  Timestamp last_mutation_at = Timestamp::unknown();

  /// Durable rack declarations, sorted by rack identity.
  struct DurableRack {
    RackId rack;
    RackGeneration generation;
    RackMembershipState membership = RackMembershipState::Unknown;
    RackPublisherId publisher;
    PublicationGeneration last_accepted_publication;
    /// Last known process incarnation. It is fenced on recovery and never
    /// restored as live authority.
    RackAgentBootId last_boot;
    RackCompositionSummary composition;
    std::vector<RackEndpoint> endpoints;
    std::vector<RackFailureDomainHint> failure_domain_hints;
    RackLifecycleState rack_lifecycle = RackLifecycleState::Unknown;
    MembershipGeneration membership_generation;
    std::string origin_label;
    Timestamp declared_at = Timestamp::unknown();

    friend bool operator==(const DurableRack&, const DurableRack&) = default;
  };
  std::vector<DurableRack> racks;

  std::vector<PlacementDomain> placement_domains;
  std::vector<CapacityDomain> capacity_domains;
  std::vector<FailureDomain> failure_domains;
  std::vector<NetworkDomain> network_domains;
  std::vector<StorageDomain> storage_domains;
  std::vector<PowerDomain> power_domains;
  std::vector<CoolingDomain> cooling_domains;
  std::vector<LinkDomain> link_domains;
  std::vector<InterRackLink> links;
  std::vector<ClusterConstraint> constraints;

  /// Rack identities that must never be silently resurrected.
  std::vector<RetiredIdentity> retired_racks;
  std::vector<RackId> withdrawn_racks;
  /// Process incarnations that stay permanently fenced.
  std::vector<FencedAuthority> fenced_authorities;

  friend bool operator==(const PersistedState&, const PersistedState&) = default;
};

struct EncodeOutcome {
  PersistenceStatus status = PersistenceStatus::Ok;
  std::string bytes;
  StructuredError error;

  [[nodiscard]] bool ok() const noexcept { return status == PersistenceStatus::Ok; }
};

/// Outcome of decoding one durable container. Distinct from the wire-codec
/// DecodeOutcome declared in protocol.hpp.
struct PersistedDecodeOutcome {
  PersistenceStatus status = PersistenceStatus::Ok;
  std::optional<PersistedState> state;
  StructuredError error;
  /// Bytes consumed before the failure, for diagnostics.
  std::uint64_t consumed = 0;

  [[nodiscard]] bool ok() const noexcept {
    return status == PersistenceStatus::Ok && state.has_value();
  }
};

/// Deterministic serialization. The same PersistedState always produces the
/// same bytes.
[[nodiscard]] EncodeOutcome encode_persisted_state(const PersistedState& state);

/// Full validation before any commit. Never returns a partially valid state.
[[nodiscard]] PersistedDecodeOutcome decode_persisted_state(std::string_view bytes);

/// Validates a decoded candidate against structural rules that do not depend
/// on live authority.
[[nodiscard]] PersistenceStatus validate_persisted_state(const PersistedState& state,
                                                        std::string* detail);

/// The durable-state storage backend.
class PersistenceStore {
 public:
  PersistenceStore() = default;
  PersistenceStore(const PersistenceStore&) = default;
  PersistenceStore(PersistenceStore&&) = default;
  PersistenceStore& operator=(const PersistenceStore&) = default;
  PersistenceStore& operator=(PersistenceStore&&) = default;
  virtual ~PersistenceStore() = default;

  struct SaveOutcome {
    bool ok = false;
    PersistenceStatus status = PersistenceStatus::Ok;
    std::uint64_t bytes_written = 0;
    StructuredError error;
  };

  struct LoadOutcome {
    PersistenceStatus status = PersistenceStatus::Ok;
    std::optional<PersistedState> state;
    StructuredError error;

    [[nodiscard]] bool ok() const noexcept {
      return status == PersistenceStatus::Ok && state.has_value();
    }
  };

  [[nodiscard]] virtual SaveOutcome save(const PersistedState& state) = 0;
  [[nodiscard]] virtual LoadOutcome load() = 0;
  /// Removes any temporary artifact. Never removes the committed container.
  virtual void discard_temporary() noexcept = 0;
};

/// File-backed store using a unique temporary file and an atomic replace.
/// Temporary names include the process id and a monotonic counter so two
/// processes cannot collide, and are always inside the destination directory
/// so the replace is a same-volume operation.
class FilePersistenceStore final : public PersistenceStore {
 public:
  explicit FilePersistenceStore(std::string path);
  ~FilePersistenceStore() override;

  FilePersistenceStore(const FilePersistenceStore&) = delete;
  FilePersistenceStore& operator=(const FilePersistenceStore&) = delete;

  [[nodiscard]] SaveOutcome save(const PersistedState& state) override;
  [[nodiscard]] LoadOutcome load() override;
  void discard_temporary() noexcept override;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] const std::string& temporary_path() const noexcept { return temporary_path_; }

 private:
  std::string path_;
  std::string temporary_path_;
  std::uint64_t temporary_counter_ = 0;
};

/// In-memory store used by tests and by in-process deployments.
class MemoryPersistenceStore final : public PersistenceStore {
 public:
  MemoryPersistenceStore() = default;

  [[nodiscard]] SaveOutcome save(const PersistedState& state) override;
  [[nodiscard]] LoadOutcome load() override;
  void discard_temporary() noexcept override;

  /// Test hook: installs raw bytes as the container content.
  void set_raw_bytes(std::string bytes);
  [[nodiscard]] const std::string& raw_bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::uint64_t save_count() const noexcept { return save_count_; }

 private:
  std::string bytes_;
  std::uint64_t save_count_ = 0;
};

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_PERSISTENCE_HPP
