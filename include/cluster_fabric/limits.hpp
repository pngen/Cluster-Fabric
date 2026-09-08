// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Hard resource bounds.
//
// Every bound below is checked before an allocation, before a reservation and
// before a count is used to size a loop. Bounds are part of the public
// contract: a peer that exceeds one receives a typed rejection rather than an
// unbounded allocation.

#ifndef CLUSTER_FABRIC_LIMITS_HPP
#define CLUSTER_FABRIC_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace cluster_fabric {

/// Maximum racks that may belong to one cluster.
inline constexpr std::size_t kMaxRacksPerCluster = 16384;

/// Maximum inter-rack links that may be declared in one topology.
inline constexpr std::size_t kMaxInterRackLinks = 262144;

/// Maximum domains of a single domain class.
inline constexpr std::size_t kMaxDomainsPerClass = 16384;

/// Maximum racks listed inside one domain record.
inline constexpr std::size_t kMaxDomainMembers = 16384;

/// Maximum failure domains referenced by one link or one rack record.
inline constexpr std::size_t kMaxFailureDomainRefs = 256;

/// Maximum cluster-level constraints retained in canonical state.
inline constexpr std::size_t kMaxConstraints = 4096;

/// Maximum rack endpoints published by one rack reference.
inline constexpr std::size_t kMaxRackEndpoints = 256;

/// Maximum capacity quantities published by one capacity domain.
inline constexpr std::size_t kMaxCapacityQuantities = 64;

/// Maximum rack-generation references retained in one snapshot map.
inline constexpr std::size_t kMaxSnapshotRackRefs = kMaxRacksPerCluster;

/// Maximum size of any protocol frame body, in bytes.
inline constexpr std::uint32_t kMaxFramePayloadBytes = 1u << 20;  // 1 MiB

/// Absolute protocol ceiling. A configured maximum may never exceed this.
inline constexpr std::uint32_t kAbsoluteMaxFramePayloadBytes = 16u << 20;  // 16 MiB

/// Maximum bytes in a persistence container.
inline constexpr std::uint64_t kMaxPersistenceBytes = 256ull << 20;  // 256 MiB

/// Maximum bytes in any single encoded string field.
inline constexpr std::size_t kMaxEncodedStringBytes = 4096;

/// Maximum records of any one kind accepted while decoding a container.
inline constexpr std::size_t kMaxDecodedRecords = 262144;

/// Maximum concurrent network sessions accepted by one coordinator.
inline constexpr std::size_t kMaxSessions = 1024;

/// Maximum requests queued for the commit thread before back pressure applies.
inline constexpr std::size_t kMaxPendingRequests = 4096;

/// Maximum snapshot objects retained for diagnostics.
inline constexpr std::size_t kMaxRetainedSnapshots = 64;

/// Maximum explanation factors produced for one decision.
inline constexpr std::size_t kMaxExplanationFactors = 64;

/// Maximum worker threads the coordinator will start.
inline constexpr std::size_t kMaxWorkerThreads = 64;

/// Maximum bytes buffered per session for outbound frames.
inline constexpr std::size_t kMaxSessionOutboundBytes = 4u << 20;  // 4 MiB

/// Maximum link domains a rack may advertise.
inline constexpr std::size_t kMaxLinkDomainsPerRack = 64;

/// Maximum retry attempts for a transient persistence commit.
inline constexpr std::uint32_t kMaxPersistenceRetries = 3;

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_LIMITS_HPP
