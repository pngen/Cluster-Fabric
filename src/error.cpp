// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Structured error construction and deterministic rendering.
//
// Rendering is a single line with fixed field order. Empty fields are omitted
// entirely, so no output ever contains a doubled separator, a trailing space or
// a dangling "field=" token.

#include "cluster_fabric/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include "cluster_fabric/limits.hpp"

namespace cluster_fabric {
namespace {

/// Appends " name=value" when p value is not empty. Values longer than the
/// encoded-string bound are truncated so a rendering can never be unbounded.
void append_field(std::string& out, std::string_view name, std::string_view value) {
  if (value.empty()) {
    return;
  }
  if (value.size() > kMaxEncodedStringBytes) {
    value = value.substr(0, kMaxEncodedStringBytes);
  }
  out.push_back(' ');
  out.append(name);
  out.push_back('=');
  out.append(value);
}

}  // namespace

std::string_view to_string(ErrorCategory value) noexcept {
  switch (value) {
    case ErrorCategory::None: return "NONE";
    case ErrorCategory::Identity: return "IDENTITY";
    case ErrorCategory::Protocol: return "PROTOCOL";
    case ErrorCategory::Persistence: return "PERSISTENCE";
    case ErrorCategory::Authority: return "AUTHORITY";
    case ErrorCategory::Generation: return "GENERATION";
    case ErrorCategory::Validation: return "VALIDATION";
    case ErrorCategory::Resource: return "RESOURCE";
    case ErrorCategory::Transport: return "TRANSPORT";
    case ErrorCategory::Shutdown: return "SHUTDOWN";
    case ErrorCategory::Internal: return "INTERNAL";
  }
  return "NONE";
}

std::string_view to_string(ErrorStage value) noexcept {
  switch (value) {
    case ErrorStage::None: return "NONE";
    case ErrorStage::Decode: return "DECODE";
    case ErrorStage::ValidateAuthority: return "VALIDATE_AUTHORITY";
    case ErrorStage::ValidateGeneration: return "VALIDATE_GENERATION";
    case ErrorStage::ValidateReference: return "VALIDATE_REFERENCE";
    case ErrorStage::ConstructCandidate: return "CONSTRUCT_CANDIDATE";
    case ErrorStage::VerifyInvariants: return "VERIFY_INVARIANTS";
    case ErrorStage::Commit: return "COMMIT";
    case ErrorStage::Persist: return "PERSIST";
    case ErrorStage::Publish: return "PUBLISH";
    case ErrorStage::Encode: return "ENCODE";
    case ErrorStage::Transport: return "TRANSPORT";
    case ErrorStage::Recover: return "RECOVER";
    case ErrorStage::Shutdown: return "SHUTDOWN";
  }
  return "NONE";
}

StructuredError StructuredError::make(ErrorCategory category, ErrorStage stage, std::string reason,
                                      std::string message) {
  StructuredError error;
  error.category = category;
  error.stage = stage;
  error.reason = std::move(reason);
  error.message = std::move(message);
  if (error.reason.size() > kMaxEncodedStringBytes) {
    error.reason.resize(kMaxEncodedStringBytes);
  }
  if (error.message.size() > kMaxEncodedStringBytes) {
    error.message.resize(kMaxEncodedStringBytes);
  }
  return error;
}

std::string StructuredError::describe() const {
  std::string out;
  out.reserve(128);
  out.append(to_string(category));
  out.push_back('/');
  out.append(to_string(stage));
  append_field(out, "subject", subject);
  append_field(out, "reason", reason);
  append_field(out, "expected", expected);
  append_field(out, "current", current);
  if (!message.empty()) {
    out.append(": ");
    if (message.size() > kMaxEncodedStringBytes) {
      out.append(std::string_view(message).substr(0, kMaxEncodedStringBytes));
    } else {
      out.append(message);
    }
  }
  return out;
}

}  // namespace cluster_fabric
