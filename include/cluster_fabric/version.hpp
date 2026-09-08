// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Product and format versioning.

#ifndef CLUSTER_FABRIC_VERSION_HPP
#define CLUSTER_FABRIC_VERSION_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace cluster_fabric {

/// Semantic version of the Cluster Fabric product.
inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

/// Stable product name used by tooling and reports.
inline constexpr std::string_view kProductName = "Cluster Fabric";

/// Stable product version text. Never localized, never reformatted.
inline constexpr std::string_view kVersionString = "1.0.0";

/// Format version of the durable persistence container. Bumped whenever the
/// on-disk layout changes in an incompatible way.
inline constexpr std::uint16_t kPersistenceFormatVersion = 1;

/// Protocol version of the framed coordinator/rack-agent transport.
inline constexpr std::uint16_t kProtocolVersion = 1;

/// Four-byte magic that opens every protocol frame.
inline constexpr std::uint32_t kProtocolMagic = 0x31465043u;  // 'C','P','F','1' little-endian

/// Four-byte magic that opens every persistence container.
inline constexpr std::uint32_t kPersistenceMagic = 0x31534643u;  // 'C','F','S','1' little-endian

/// True when the supplied major/minor pair can be decoded by this build.
[[nodiscard]] constexpr bool is_compatible_format(std::uint16_t format_version) noexcept {
  return format_version == kPersistenceFormatVersion;
}

[[nodiscard]] constexpr std::string_view version_string() noexcept { return kVersionString; }
[[nodiscard]] constexpr std::string_view product_name() noexcept { return kProductName; }

/// Compiler/architecture identity of this translation unit set. Deterministic
/// for a given toolchain and used in reports and the CLI banner.
[[nodiscard]] std::string_view build_identity() noexcept;

/// Single-line human-readable banner: "Cluster Fabric 1.0.0 (<build>)".
[[nodiscard]] std::string version_banner();

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_VERSION_HPP
