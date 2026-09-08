// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic structured explanations.
//
// Factors are ordered by (code, subject, detail) so that two explanations built
// from the same facts render byte-identically. The factor list is bounded:
// add() keeps a single slot of slack above the public bound so bound_factors()
// can observe an overflow and report it instead of silently dropping facts.

#include "cluster_fabric/explanation.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "cluster_fabric/limits.hpp"

namespace cluster_fabric {
namespace {

/// One slot above the public bound: enough for bound_factors() to detect that
/// truncation is required, never enough to grow without limit.
constexpr std::size_t kFactorCapacity = kMaxExplanationFactors + 1u;

constexpr std::string_view kTruncationCode = "truncated";
constexpr std::string_view kTruncationDetail = "additional factors omitted";

void append_bounded(std::string& out, std::string_view value) {
  if (value.size() > kMaxEncodedStringBytes) {
    value = value.substr(0, kMaxEncodedStringBytes);
  }
  out.append(value);
}

void append_named(std::string& out, std::string_view name, std::string_view value) {
  if (value.empty()) {
    return;
  }
  if (!out.empty()) {
    out.push_back(' ');
  }
  out.append(name);
  out.push_back('=');
  append_bounded(out, value);
}

/// Renders one factor as "  <code> <subject>: <detail>", omitting empty fields
/// so the line never contains a doubled separator.
[[nodiscard]] std::string render_factor(const ExplanationFactor& factor) {
  std::string line = "  ";
  bool wrote_identity = false;
  if (!factor.code.empty()) {
    append_bounded(line, factor.code);
    wrote_identity = true;
  }
  if (!factor.subject.empty()) {
    if (wrote_identity) {
      line.push_back(' ');
    }
    append_bounded(line, factor.subject);
    wrote_identity = true;
  }
  if (!factor.detail.empty()) {
    if (wrote_identity) {
      line.append(": ");
    }
    append_bounded(line, factor.detail);
  }
  return line;
}

}  // namespace

Explanation Explanation::make(std::string code_text, std::string subject_text,
                              std::string summary_text) {
  Explanation explanation;
  explanation.code = std::move(code_text);
  explanation.subject = std::move(subject_text);
  explanation.summary = std::move(summary_text);
  return explanation;
}

void Explanation::add(std::string code_text, std::string subject_text, std::string detail_text) {
  if (factors.size() >= kFactorCapacity) {
    return;
  }
  ExplanationFactor factor;
  factor.code = std::move(code_text);
  factor.subject = std::move(subject_text);
  factor.detail = std::move(detail_text);
  factors.push_back(std::move(factor));
}

void Explanation::sort_factors() {
  std::sort(factors.begin(), factors.end(),
            [](const ExplanationFactor& lhs, const ExplanationFactor& rhs) {
              if (lhs.code != rhs.code) {
                return lhs.code < rhs.code;
              }
              if (lhs.subject != rhs.subject) {
                return lhs.subject < rhs.subject;
              }
              return lhs.detail < rhs.detail;
            });
}

void Explanation::bound_factors() {
  if (factors.size() <= kMaxExplanationFactors) {
    return;
  }
  // Reserve one slot for the truncation marker when the bound allows it, so
  // the rendered explanation is both bounded and explicit about the loss.
  const std::size_t keep =
      kMaxExplanationFactors > 0 ? kMaxExplanationFactors - 1u : static_cast<std::size_t>(0);
  factors.resize(keep);
  if (factors.size() < kMaxExplanationFactors) {
    ExplanationFactor marker;
    marker.code = std::string(kTruncationCode);
    marker.detail = std::string(kTruncationDetail);
    factors.push_back(std::move(marker));
  }
}

std::string Explanation::describe() const {
  std::string out;
  out.reserve(128);
  append_named(out, "code", code);
  append_named(out, "subject", subject);
  if (!summary.empty()) {
    if (!out.empty()) {
      out.append(": ");
    }
    append_bounded(out, summary);
  }

  const std::size_t rendered = std::min(factors.size(), kMaxExplanationFactors);
  for (std::size_t i = 0; i < rendered; ++i) {
    out.push_back('\n');
    out.append(render_factor(factors[i]));
  }
  return out;
}

}  // namespace cluster_fabric
