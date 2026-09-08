// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Strongly typed identities.
//
// Cluster Fabric never represents unrelated identities as interchangeable raw
// integers or strings. A ClusterId cannot be passed where a RackId is
// expected, and neither can be silently converted to an integer.
//
// Textual identities are bounded and validated at the point of construction.
// The accepted alphabet is deliberately narrow: ASCII letters, digits and the
// separators '-', '_', '.', ':' and '@'. Whitespace, control characters,
// non-ASCII bytes and path separators are rejected, so an identity can never
// be used to traverse a path or to inject framing.

#ifndef CLUSTER_FABRIC_IDENTITY_HPP
#define CLUSTER_FABRIC_IDENTITY_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "cluster_fabric/limits.hpp"

namespace cluster_fabric {

/// Maximum number of bytes in any textual identity accepted from a peer, a
/// file or an API caller.
inline constexpr std::size_t kMaxIdentityLength = 128;

enum class IdentityStatus {
  Ok = 0,
  Empty,
  TooLong,
  InvalidCharacter,
  Reserved,
};

struct IdentityValidation {
  IdentityStatus status = IdentityStatus::Ok;
  std::size_t position = 0;

  [[nodiscard]] constexpr bool ok() const noexcept { return status == IdentityStatus::Ok; }
};

[[nodiscard]] std::string_view to_string(IdentityStatus value) noexcept;

[[nodiscard]] constexpr bool is_identity_char(char c) noexcept {
  const unsigned char u = static_cast<unsigned char>(c);
  if (u >= '0' && u <= '9') return true;
  if (u >= 'a' && u <= 'z') return true;
  if (u >= 'A' && u <= 'Z') return true;
  switch (c) {
    case '-':
    case '_':
    case '.':
    case ':':
    case '@':
      return true;
    default:
      return false;
  }
}

/// Validates a textual identity. Pure and allocation free so it can be used on
/// untrusted input before any memory is reserved for it.
[[nodiscard]] constexpr IdentityValidation validate_identity(std::string_view value) noexcept {
  if (value.empty()) {
    return IdentityValidation{IdentityStatus::Empty, 0};
  }
  if (value.size() > kMaxIdentityLength) {
    return IdentityValidation{IdentityStatus::TooLong, kMaxIdentityLength};
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (!is_identity_char(value[i])) {
      return IdentityValidation{IdentityStatus::InvalidCharacter, i};
    }
  }
  if (value == "." || value == "..") {
    return IdentityValidation{IdentityStatus::Reserved, 0};
  }
  const std::size_t double_dot = value.find("..");
  if (double_dot != std::string_view::npos) {
    return IdentityValidation{IdentityStatus::Reserved, double_dot};
  }
  return IdentityValidation{};
}

/// Validation for free-form labels (model names, vendor names, operator
/// notes). Spaces are allowed; control characters and non-ASCII are not.
[[nodiscard]] constexpr IdentityValidation validate_label(std::string_view value) noexcept {
  if (value.empty()) {
    return IdentityValidation{IdentityStatus::Empty, 0};
  }
  if (value.size() > kMaxIdentityLength) {
    return IdentityValidation{IdentityStatus::TooLong, kMaxIdentityLength};
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    const unsigned char u = static_cast<unsigned char>(value[i]);
    if (u < 0x20 || u > 0x7E) {
      return IdentityValidation{IdentityStatus::InvalidCharacter, i};
    }
  }
  return IdentityValidation{};
}

/// A distinct, validated textual identity type. Tag supplies the type
/// identity; only tags declared in this header are ever used.
template <class Tag>
class StrongId {
 public:
  /// Default construction yields the explicit UNKNOWN identity: an empty
  /// value that no parser accepts and that never compares equal to a parsed
  /// identity. Structures that must be default-constructible (request and
  /// message aggregates, queues) can therefore exist without ever holding a
  /// fabricated identity, and every validity check still rejects them.
  StrongId() = default;
  StrongId(const StrongId&) = default;
  StrongId(StrongId&&) noexcept = default;
  StrongId& operator=(const StrongId&) = default;
  StrongId& operator=(StrongId&&) noexcept = default;
  ~StrongId() = default;

  /// Returns std::nullopt when the text is not a valid identity.
  [[nodiscard]] static std::optional<StrongId> parse(std::string_view value) {
    if (!validate_identity(value).ok()) {
      return std::nullopt;
    }
    return StrongId(std::string(value));
  }

  [[nodiscard]] static bool is_valid(std::string_view value) noexcept {
    return validate_identity(value).ok();
  }

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  /// False for the UNKNOWN (default-constructed) identity.
  [[nodiscard]] bool known() const noexcept { return !value_.empty(); }

  friend bool operator==(const StrongId& lhs, const StrongId& rhs) = default;
  friend auto operator<=>(const StrongId& lhs, const StrongId& rhs) = default;

  [[nodiscard]] std::size_t hash() const noexcept { return std::hash<std::string>{}(value_); }

 private:
  explicit StrongId(std::string value) : value_(std::move(value)) {}
  std::string value_;
};

struct ClusterIdTag;
struct RackIdTag;
struct RackAgentBootIdTag;
struct RackPublisherIdTag;
struct RackEndpointIdTag;
struct PlacementDomainIdTag;
struct CapacityDomainIdTag;
struct FailureDomainIdTag;
struct NetworkDomainIdTag;
struct StorageDomainIdTag;
struct PowerDomainIdTag;
struct CoolingDomainIdTag;
struct LinkDomainIdTag;
struct InterRackLinkIdTag;
struct ConstraintIdTag;

/// Identity of one cluster. Cluster Fabric is scoped to exactly one of these.
using ClusterId = StrongId<ClusterIdTag>;
/// Identity of a rack as owned by Rack Fabric. Cluster Fabric references it.
using RackId = StrongId<RackIdTag>;
/// Process incarnation identity of a rack-side publisher. Fencing depends on it.
using RackAgentBootId = StrongId<RackAgentBootIdTag>;
/// Durable identity of the authority entitled to publish a rack.
using RackPublisherId = StrongId<RackPublisherIdTag>;
/// Identity of a rack endpoint usable for inter-rack connectivity.
using RackEndpointId = StrongId<RackEndpointIdTag>;
using PlacementDomainId = StrongId<PlacementDomainIdTag>;
using CapacityDomainId = StrongId<CapacityDomainIdTag>;
using FailureDomainId = StrongId<FailureDomainIdTag>;
using NetworkDomainId = StrongId<NetworkDomainIdTag>;
using StorageDomainId = StrongId<StorageDomainIdTag>;
using PowerDomainId = StrongId<PowerDomainIdTag>;
using CoolingDomainId = StrongId<CoolingDomainIdTag>;
/// Grouping of links that fail together (for example one fabric plane).
using LinkDomainId = StrongId<LinkDomainIdTag>;
/// Identity of one directed or bidirectional inter-rack relationship.
using InterRackLinkId = StrongId<InterRackLinkIdTag>;
/// Identity of a declared cluster-level constraint.
using ConstraintId = StrongId<ConstraintIdTag>;

/// Generates a fresh process-incarnation identity. The value is opaque, but
/// contains the process id and a monotonic component so that two incarnations
/// of the same process id are still distinguishable.
[[nodiscard]] std::string make_boot_id_text(std::string_view prefix);

/// Convenience: a fresh RackAgentBootId.
[[nodiscard]] RackAgentBootId make_rack_agent_boot_id(std::string_view prefix = "boot");

}  // namespace cluster_fabric

namespace std {

template <class Tag>
struct hash<cluster_fabric::StrongId<Tag>> {
  [[nodiscard]] size_t operator()(const cluster_fabric::StrongId<Tag>& value) const noexcept {
    return value.hash();
  }
};

}  // namespace std

#endif  // CLUSTER_FABRIC_IDENTITY_HPP
