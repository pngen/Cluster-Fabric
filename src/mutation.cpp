// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Mutation request rendering and typed results.

#include "cluster_fabric/mutation.hpp"

#include <string>
#include <utility>

namespace cluster_fabric {

std::string_view to_string(MutationKind value) noexcept {
  switch (value) {
    case MutationKind::Unknown: return "UNKNOWN";
    case MutationKind::DeclareCluster: return "DECLARE_CLUSTER";
    case MutationKind::RegisterRackPublisher: return "REGISTER_RACK_PUBLISHER";
    case MutationKind::AddRack: return "ADD_RACK";
    case MutationKind::UpdateRackGeneration: return "UPDATE_RACK_GENERATION";
    case MutationKind::MarkRackUnavailable: return "MARK_RACK_UNAVAILABLE";
    case MutationKind::WithdrawRack: return "WITHDRAW_RACK";
    case MutationKind::RetireRack: return "RETIRE_RACK";
    case MutationKind::RecoverRack: return "RECOVER_RACK";
    case MutationKind::PublishInterRackLink: return "PUBLISH_INTER_RACK_LINK";
    case MutationKind::WithdrawInterRackLink: return "WITHDRAW_INTER_RACK_LINK";
    case MutationKind::PublishPlacementDomain: return "PUBLISH_PLACEMENT_DOMAIN";
    case MutationKind::PublishCapacityDomain: return "PUBLISH_CAPACITY_DOMAIN";
    case MutationKind::PublishFailureDomain: return "PUBLISH_FAILURE_DOMAIN";
    case MutationKind::PublishNetworkDomain: return "PUBLISH_NETWORK_DOMAIN";
    case MutationKind::PublishStorageDomain: return "PUBLISH_STORAGE_DOMAIN";
    case MutationKind::PublishPowerDomain: return "PUBLISH_POWER_DOMAIN";
    case MutationKind::PublishCoolingDomain: return "PUBLISH_COOLING_DOMAIN";
    case MutationKind::PublishLinkDomain: return "PUBLISH_LINK_DOMAIN";
    case MutationKind::PublishConstraint: return "PUBLISH_CONSTRAINT";
    case MutationKind::PublishHealthCurrentness: return "PUBLISH_HEALTH_CURRENTNESS";
    case MutationKind::WithdrawEvidence: return "WITHDRAW_EVIDENCE";
    case MutationKind::SupersedeTopology: return "SUPERSEDE_TOPOLOGY";
    case MutationKind::PublishSnapshot: return "PUBLISH_SNAPSHOT";
    case MutationKind::RevalidateRecoveredState: return "REVALIDATE_RECOVERED_STATE";
    case MutationKind::RetireCluster: return "RETIRE_CLUSTER";
  }
  return "UNKNOWN";
}

std::optional<MutationKind> mutation_kind_from_string(std::string_view text) noexcept {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(MutationKind::RetireCluster); ++raw) {
    const auto kind = static_cast<MutationKind>(raw);
    if (to_string(kind) == text) {
      return kind;
    }
  }
  return std::nullopt;
}

std::string_view to_string(MutationOutcome value) noexcept {
  switch (value) {
    case MutationOutcome::Accepted: return "ACCEPTED";
    case MutationOutcome::NoChange: return "NO_CHANGE";
    case MutationOutcome::Rejected: return "REJECTED";
    case MutationOutcome::RevalidationRequired: return "REVALIDATION_REQUIRED";
  }
  return "REJECTED";
}

std::string_view to_string(RejectionReason value) noexcept {
  switch (value) {
    case RejectionReason::None: return "NONE";
    case RejectionReason::StaleClusterEpoch: return "REJECT_STALE_CLUSTER_EPOCH";
    case RejectionReason::StaleCoordinatorEpoch: return "REJECT_STALE_COORDINATOR_EPOCH";
    case RejectionReason::StaleRackBoot: return "REJECT_STALE_RACK_BOOT";
    case RejectionReason::StaleRackGeneration: return "REJECT_STALE_RACK_GENERATION";
    case RejectionReason::StaleTopologyEpoch: return "REJECT_STALE_TOPOLOGY_EPOCH";
    case RejectionReason::StalePublication: return "REJECT_STALE_PUBLICATION";
    case RejectionReason::NotAuthorized: return "REJECT_NOT_AUTHORIZED";
    case RejectionReason::Conflict: return "REJECT_CONFLICT";
    case RejectionReason::UnknownRack: return "REJECT_UNKNOWN_RACK";
    case RejectionReason::UnknownDomain: return "REJECT_UNKNOWN_DOMAIN";
    case RejectionReason::InvalidDomain: return "REJECT_INVALID_DOMAIN";
    case RejectionReason::InvalidRelationship: return "REJECT_INVALID_RELATIONSHIP";
    case RejectionReason::Retired: return "REJECT_RETIRED";
    case RejectionReason::WrongCluster: return "REJECT_WRONG_CLUSTER";
    case RejectionReason::LimitExceeded: return "REJECT_LIMIT_EXCEEDED";
    case RejectionReason::Malformed: return "REJECT_MALFORMED";
    case RejectionReason::NotReady: return "REJECT_NOT_READY";
    case RejectionReason::ShuttingDown: return "REJECT_SHUTTING_DOWN";
    case RejectionReason::PersistenceFailed: return "REJECT_PERSISTENCE_FAILED";
    case RejectionReason::InvariantViolation: return "REJECT_INVARIANT_VIOLATION";
    case RejectionReason::Internal: return "REJECT_INTERNAL";
  }
  return "REJECT_INTERNAL";
}

std::optional<RejectionReason> rejection_reason_from_string(std::string_view text) noexcept {
  for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(RejectionReason::Internal); ++raw) {
    const auto reason = static_cast<RejectionReason>(raw);
    if (to_string(reason) == text) {
      return reason;
    }
  }
  return std::nullopt;
}

std::string MutationRequest::describe() const {
  std::string out = "kind=";
  out += to_string(kind);
  out += " cluster=";
  out += cluster.view();
  out += " cluster_epoch=";
  out += authority.cluster_epoch.str();
  out += " coordinator_epoch=";
  out += authority.coordinator_epoch.str();
  out += " topology_epoch=";
  out += authority.topology_epoch.str();
  if (authority.rack.has_value()) {
    out += " rack=";
    out += authority.rack->view();
  }
  if (authority.rack_generation.has_value()) {
    out += " rack_generation=";
    out += authority.rack_generation->str();
  }
  if (authority.boot.has_value()) {
    out += " boot=";
    out += authority.boot->view();
  }
  if (authority.publisher.has_value()) {
    out += " publisher=";
    out += authority.publisher->view();
  }
  out += " publication=";
  out += authority.publication.str();
  return out;
}

MutationResult MutationResult::rejected(RejectionReason reason, ErrorStage stage,
                                        std::string subject, std::string detail,
                                        std::string reason_code) {
  MutationResult result;
  result.outcome = MutationOutcome::Rejected;
  result.reason = reason;
  if (reason_code.empty()) {
    reason_code = std::string(to_string(reason));
  }
  ErrorCategory category = ErrorCategory::Validation;
  switch (reason) {
    case RejectionReason::StaleClusterEpoch:
    case RejectionReason::StaleCoordinatorEpoch:
    case RejectionReason::StaleRackBoot:
    case RejectionReason::StaleRackGeneration:
    case RejectionReason::StaleTopologyEpoch:
    case RejectionReason::StalePublication:
      category = ErrorCategory::Generation;
      break;
    case RejectionReason::NotAuthorized:
    case RejectionReason::Conflict:
    case RejectionReason::Retired:
      category = ErrorCategory::Authority;
      break;
    case RejectionReason::LimitExceeded:
      category = ErrorCategory::Resource;
      break;
    case RejectionReason::PersistenceFailed:
      category = ErrorCategory::Persistence;
      break;
    case RejectionReason::ShuttingDown:
      category = ErrorCategory::Shutdown;
      break;
    case RejectionReason::Internal:
    case RejectionReason::InvariantViolation:
      category = ErrorCategory::Internal;
      break;
    default:
      category = ErrorCategory::Validation;
      break;
  }
  result.error = StructuredError::make(category, stage, std::move(reason_code), std::move(detail));
  result.error.subject = std::move(subject);
  result.explanation = Explanation::make(std::string(to_string(reason)), result.error.subject,
                                         result.error.message);
  return result;
}

}  // namespace cluster_fabric
