// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic structured explanations.
//
// Every decision that a caller may need to justify returns a structured
// explanation. Callers never have to parse logs to learn why a mutation was
// rejected, why a rack is excluded, or why a snapshot is stale.

#ifndef CLUSTER_FABRIC_EXPLANATION_HPP
#define CLUSTER_FABRIC_EXPLANATION_HPP

#include <string>
#include <string_view>
#include <vector>

#include "cluster_fabric/error.hpp"
#include "cluster_fabric/generation.hpp"

namespace cluster_fabric {

/// One contributing factor. Codes are stable identifiers, not prose.
struct ExplanationFactor {
  std::string code;
  std::string subject;
  std::string detail;

  friend bool operator==(const ExplanationFactor&, const ExplanationFactor&) = default;
};

/// A complete explanation of one decision.
struct Explanation {
  /// Stable decision code, for example "cluster_not_ready".
  std::string code;
  /// Identity the decision is about.
  std::string subject;
  /// Human-readable summary.
  std::string summary;
  /// Factors sorted by (code, subject) so output is deterministic.
  std::vector<ExplanationFactor> factors;

  [[nodiscard]] static Explanation make(std::string code, std::string subject, std::string summary);

  void add(std::string code, std::string subject, std::string detail);
  void sort_factors();
  void bound_factors();

  [[nodiscard]] bool empty() const noexcept { return code.empty(); }

  /// Deterministic multi-line rendering used by the CLI and by tests.
  [[nodiscard]] std::string describe() const;

  friend bool operator==(const Explanation&, const Explanation&) = default;
};

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_EXPLANATION_HPP
