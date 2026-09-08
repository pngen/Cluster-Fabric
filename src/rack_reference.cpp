// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Validation and canonicalization of the narrow rack contract.

#include "cluster_fabric/rack_reference.hpp"

#include <algorithm>
#include <string>

#include "cluster_fabric/limits.hpp"

namespace cluster_fabric {

std::string_view to_string(RackReferenceIssue value) noexcept {
  switch (value) {
    case RackReferenceIssue::None: return "NONE";
    case RackReferenceIssue::MissingRackId: return "MISSING_RACK_ID";
    case RackReferenceIssue::MissingGeneration: return "MISSING_GENERATION";
    case RackReferenceIssue::TooManyEndpoints: return "TOO_MANY_ENDPOINTS";
    case RackReferenceIssue::TooManyFailureDomainHints: return "TOO_MANY_FAILURE_DOMAIN_HINTS";
    case RackReferenceIssue::TooManyAcceleratorClasses: return "TOO_MANY_ACCELERATOR_CLASSES";
    case RackReferenceIssue::InvalidLabel: return "INVALID_LABEL";
    case RackReferenceIssue::DuplicateEndpoint: return "DUPLICATE_ENDPOINT";
    case RackReferenceIssue::DuplicateFailureDomainHint: return "DUPLICATE_FAILURE_DOMAIN_HINT";
    case RackReferenceIssue::MissingPublisher: return "MISSING_PUBLISHER";
    case RackReferenceIssue::UnknownProvenance: return "UNKNOWN_PROVENANCE";
  }
  return "UNKNOWN";
}

namespace {

[[nodiscard]] RackReferenceValidation issue(RackReferenceIssue issue_value, std::string subject,
                                            std::string detail) {
  RackReferenceValidation validation;
  validation.issue = issue_value;
  validation.subject = std::move(subject);
  validation.detail = std::move(detail);
  return validation;
}

}  // namespace

RackReferenceValidation validate_rack_reference(const RackReference& reference) {
  if (reference.rack.view().empty()) {
    return issue(RackReferenceIssue::MissingRackId, "rack", "rack identity is empty");
  }
  if (!reference.generation.known()) {
    return issue(RackReferenceIssue::MissingGeneration, "rack/" + reference.rack.value(),
                 "rack generation is zero (unknown)");
  }
  if (reference.endpoints.size() > kMaxRackEndpoints) {
    return issue(RackReferenceIssue::TooManyEndpoints, "rack/" + reference.rack.value(),
                 "endpoint count " + std::to_string(reference.endpoints.size()) + " exceeds " +
                     std::to_string(kMaxRackEndpoints));
  }
  if (reference.failure_domain_hints.size() > kMaxFailureDomainRefs) {
    return issue(RackReferenceIssue::TooManyFailureDomainHints, "rack/" + reference.rack.value(),
                 "failure domain hint count " +
                     std::to_string(reference.failure_domain_hints.size()) + " exceeds " +
                     std::to_string(kMaxFailureDomainRefs));
  }
  if (reference.composition.accelerators.size() > kMaxCapacityQuantities) {
    return issue(RackReferenceIssue::TooManyAcceleratorClasses, "rack/" + reference.rack.value(),
                 "accelerator class count " +
                     std::to_string(reference.composition.accelerators.size()) + " exceeds " +
                     std::to_string(kMaxCapacityQuantities));
  }
  if (!validate_label(reference.composition.composition_label).ok() &&
      !reference.composition.composition_label.empty()) {
    return issue(RackReferenceIssue::InvalidLabel, "rack/" + reference.rack.value(),
                 "composition label is not a valid bounded label");
  }
  if (!validate_label(reference.origin_label).ok() && !reference.origin_label.empty()) {
    return issue(RackReferenceIssue::InvalidLabel, "rack/" + reference.rack.value(),
                 "origin label is not a valid bounded label");
  }
  for (const AcceleratorClassSummary& summary : reference.composition.accelerators) {
    if (!summary.family.empty() && !validate_label(summary.family).ok()) {
      return issue(RackReferenceIssue::InvalidLabel, "rack/" + reference.rack.value(),
                   "accelerator family label is not a valid bounded label");
    }
  }
  for (const RackFailureDomainHint& hint : reference.failure_domain_hints) {
    if (hint.klass == FailureDomainClass::Unknown) {
      return issue(RackReferenceIssue::InvalidLabel, "rack/" + reference.rack.value(),
                   "failure domain hint has UNKNOWN class");
    }
  }
  for (const RackEndpoint& endpoint : reference.endpoints) {
    if (endpoint.id.view().empty()) {
      return issue(RackReferenceIssue::InvalidLabel, "rack/" + reference.rack.value(),
                   "endpoint identity is empty");
    }
  }
  if (!reference.publisher.has_value()) {
    return issue(RackReferenceIssue::MissingPublisher, "rack/" + reference.rack.value(),
                 "publisher authority is absent");
  }
  if (reference.evidence.provenance == EvidenceProvenance::Unknown) {
    return issue(RackReferenceIssue::UnknownProvenance, "rack/" + reference.rack.value(),
                 "reference provenance is UNKNOWN");
  }
  return RackReferenceValidation{};
}

RackReference canonicalize(RackReference reference) {
  std::sort(reference.composition.accelerators.begin(), reference.composition.accelerators.end());
  reference.composition.accelerators.erase(
      std::unique(reference.composition.accelerators.begin(),
                  reference.composition.accelerators.end()),
      reference.composition.accelerators.end());

  std::sort(reference.endpoints.begin(), reference.endpoints.end(),
            [](const RackEndpoint& lhs, const RackEndpoint& rhs) { return lhs.id < rhs.id; });
  reference.endpoints.erase(
      std::unique(reference.endpoints.begin(), reference.endpoints.end(),
                  [](const RackEndpoint& lhs, const RackEndpoint& rhs) {
                    return lhs.id == rhs.id;
                  }),
      reference.endpoints.end());

  std::sort(reference.failure_domain_hints.begin(), reference.failure_domain_hints.end(),
            [](const RackFailureDomainHint& lhs, const RackFailureDomainHint& rhs) {
              if (lhs.klass != rhs.klass) {
                return lhs.klass < rhs.klass;
              }
              return lhs.id < rhs.id;
            });
  reference.failure_domain_hints.erase(
      std::unique(reference.failure_domain_hints.begin(), reference.failure_domain_hints.end(),
                  [](const RackFailureDomainHint& lhs, const RackFailureDomainHint& rhs) {
                    return lhs.klass == rhs.klass && lhs.id == rhs.id;
                  }),
      reference.failure_domain_hints.end());
  return reference;
}

}  // namespace cluster_fabric
