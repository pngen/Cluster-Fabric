// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Structured error contract.
//
// Every material failure carries an error category, the operation stage that
// failed, the affected identity, the expected and current generation or epoch
// where those exist, a machine-readable reason code and a human-readable
// explanation. Raw implementation exceptions are never the public contract.

#ifndef CLUSTER_FABRIC_ERROR_HPP
#define CLUSTER_FABRIC_ERROR_HPP

#include <cstdint>
#include <string>
#include <string_view>

namespace cluster_fabric {

enum class ErrorCategory : std::uint8_t {
  None = 0,
  Identity,
  Protocol,
  Persistence,
  Authority,
  Generation,
  Validation,
  Resource,
  Transport,
  Shutdown,
  Internal,
};

/// The stage of the transactional mutation sequence in which a failure was
/// detected. The order mirrors the commit pipeline exactly.
enum class ErrorStage : std::uint8_t {
  None = 0,
  Decode,
  ValidateAuthority,
  ValidateGeneration,
  ValidateReference,
  ConstructCandidate,
  VerifyInvariants,
  Commit,
  Persist,
  Publish,
  Encode,
  Transport,
  Recover,
  Shutdown,
};

[[nodiscard]] std::string_view to_string(ErrorCategory value) noexcept;
[[nodiscard]] std::string_view to_string(ErrorStage value) noexcept;

/// A complete, structured failure description.
struct StructuredError {
  ErrorCategory category = ErrorCategory::None;
  ErrorStage stage = ErrorStage::None;
  /// Affected identity, for example "rack/A" or "domain/fd/row-1".
  std::string subject;
  /// Machine-readable reason code, for example "stale_rack_generation".
  std::string reason;
  /// Expected authoritative value, for example "cluster_epoch=7".
  std::string expected;
  /// Value actually supplied, for example "cluster_epoch=6".
  std::string current;
  /// Human-readable explanation. Never contains raw exception text.
  std::string message;

  [[nodiscard]] bool ok() const noexcept { return category == ErrorCategory::None; }

  [[nodiscard]] static StructuredError none() noexcept { return StructuredError{}; }

  [[nodiscard]] static StructuredError make(ErrorCategory category, ErrorStage stage,
                                            std::string reason, std::string message);

  /// Deterministic single-line rendering used by the CLI and by logs.
  [[nodiscard]] std::string describe() const;
};

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_ERROR_HPP
