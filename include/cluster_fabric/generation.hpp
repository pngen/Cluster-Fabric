// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Monotonic generations and epochs.
//
// Every externally significant mutation is bound to the generations and
// epochs that were current when it was authored. A stale value is always
// rejected before it can mutate authoritative state. Zero means "unknown" and
// is never a valid authoritative value.

#ifndef CLUSTER_FABRIC_GENERATION_HPP
#define CLUSTER_FABRIC_GENERATION_HPP

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace cluster_fabric {

/// A monotonically increasing, strongly typed counter. Zero is reserved for
/// "unknown / never published".
template <class Tag>
class StrongCounter {
 public:
  using value_type = std::uint64_t;

  constexpr StrongCounter() noexcept = default;
  explicit constexpr StrongCounter(value_type value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongCounter from_raw(value_type value) noexcept {
    return StrongCounter(value);
  }

  [[nodiscard]] constexpr value_type value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool known() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

  /// Checked successor. Returns std::nullopt on overflow rather than wrapping.
  [[nodiscard]] std::optional<StrongCounter> next() const noexcept {
    if (value_ == std::numeric_limits<value_type>::max()) {
      return std::nullopt;
    }
    return StrongCounter(value_ + 1);
  }

  /// True when p candidate is a strictly newer value than this one.
  [[nodiscard]] constexpr bool precedes(const StrongCounter& candidate) const noexcept {
    return candidate.value_ > value_;
  }

  friend constexpr bool operator==(const StrongCounter&, const StrongCounter&) noexcept = default;
  friend constexpr auto operator<=>(const StrongCounter&, const StrongCounter&) noexcept = default;

  [[nodiscard]] std::size_t hash() const noexcept {
    return std::hash<value_type>{}(value_);
  }

  [[nodiscard]] std::string str() const { return std::to_string(value_); }

 private:
  value_type value_ = 0;
};

struct ClusterEpochTag;
struct CoordinatorEpochTag;
struct ClusterGenerationTag;
struct MembershipGenerationTag;
struct TopologyEpochTag;
struct TopologyGenerationTag;
struct ConnectivityGenerationTag;
struct HealthGenerationTag;
struct ConstraintGenerationTag;
struct SnapshotGenerationTag;
struct PublicationGenerationTag;
struct RackGenerationTag;
struct DomainGenerationTag;

/// Incarnation epoch of the cluster itself. Advanced when the cluster is
/// re-declared under a fresh authority or recovered into a new incarnation.
using ClusterEpoch = StrongCounter<ClusterEpochTag>;
/// Incarnation epoch of the coordinator process set. Advanced by every
/// coordinator restart. Messages authored under an older epoch are fenced.
using CoordinatorEpoch = StrongCounter<CoordinatorEpochTag>;
/// Monotonic generation of the authoritative cluster composition.
using ClusterGeneration = StrongCounter<ClusterGenerationTag>;
/// Monotonic generation of the rack-membership map.
using MembershipGeneration = StrongCounter<MembershipGenerationTag>;
/// One coherent authoritative cluster-topology regime.
using TopologyEpoch = StrongCounter<TopologyEpochTag>;
/// Monotonic generation of topology content within a topology epoch.
using TopologyGeneration = StrongCounter<TopologyGenerationTag>;
using ConnectivityGeneration = StrongCounter<ConnectivityGenerationTag>;
using HealthGeneration = StrongCounter<HealthGenerationTag>;
using ConstraintGeneration = StrongCounter<ConstraintGenerationTag>;
using SnapshotGeneration = StrongCounter<SnapshotGenerationTag>;
using PublicationGeneration = StrongCounter<PublicationGenerationTag>;
/// Authoritative generation of one rack's composition, owned by Rack Fabric.
using RackGeneration = StrongCounter<RackGenerationTag>;
/// Generation of one domain record or of a domain class as a whole.
using DomainGeneration = StrongCounter<DomainGenerationTag>;

/// Parses a decimal counter value from untrusted text. Rejects signs,
/// whitespace, overflow and the reserved zero value when p allow_zero is
/// false.
[[nodiscard]] std::optional<std::uint64_t> parse_counter_text(std::string_view text,
                                                             bool allow_zero) noexcept;

[[nodiscard]] std::string_view to_string(ClusterEpoch value) noexcept;
[[nodiscard]] std::string_view to_string(TopologyEpoch value) noexcept;

}  // namespace cluster_fabric

namespace std {

template <class Tag>
struct hash<cluster_fabric::StrongCounter<Tag>> {
  [[nodiscard]] size_t operator()(const cluster_fabric::StrongCounter<Tag>& value) const noexcept {
    return value.hash();
  }
};

}  // namespace std

#endif  // CLUSTER_FABRIC_GENERATION_HPP
