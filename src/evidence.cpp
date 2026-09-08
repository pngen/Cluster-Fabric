// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Rendering, parsing and classification of evidence metadata.
//
// Canonical spellings are fixed and never localized. Parsing accepts the
// canonical spelling and its TitleCase enumerator form and nothing else, so a
// misspelled or partially matching provenance is rejected rather than guessed.

#include "cluster_fabric/evidence.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cluster_fabric {
namespace {

template <class Enum>
struct EnumName {
  Enum value;
  std::string_view canonical;
  std::string_view title_case;
};

constexpr std::array<EnumName<EvidenceProvenance>, 7> kProvenanceNames{{
    {EvidenceProvenance::Unknown, "UNKNOWN", "Unknown"},
    {EvidenceProvenance::Measured, "MEASURED", "Measured"},
    {EvidenceProvenance::Reported, "REPORTED", "Reported"},
    {EvidenceProvenance::Derived, "DERIVED", "Derived"},
    {EvidenceProvenance::Estimated, "ESTIMATED", "Estimated"},
    {EvidenceProvenance::Synthetic, "SYNTHETIC", "Synthetic"},
    {EvidenceProvenance::Reconstructed, "RECONSTRUCTED", "Reconstructed"},
}};

constexpr std::array<EnumName<Freshness>, 4> kFreshnessNames{{
    {Freshness::Unknown, "UNKNOWN", "Unknown"},
    {Freshness::Fresh, "FRESH", "Fresh"},
    {Freshness::Stale, "STALE", "Stale"},
    {Freshness::RevalidationRequired, "REVALIDATION_REQUIRED", "RevalidationRequired"},
}};

/// Process wall clock backed by std::chrono::system_clock.
class SystemClock final : public Clock {
 public:
  SystemClock() = default;
  SystemClock(const SystemClock&) = delete;
  SystemClock(SystemClock&&) = delete;
  SystemClock& operator=(const SystemClock&) = delete;
  SystemClock& operator=(SystemClock&&) = delete;
  ~SystemClock() override = default;

  [[nodiscard]] Timestamp now() const override {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
    return Timestamp::from_unix_millis(static_cast<std::int64_t>(millis));
  }
};

}  // namespace

std::string_view to_string(EvidenceProvenance value) noexcept {
  switch (value) {
    case EvidenceProvenance::Unknown: return "UNKNOWN";
    case EvidenceProvenance::Measured: return "MEASURED";
    case EvidenceProvenance::Reported: return "REPORTED";
    case EvidenceProvenance::Derived: return "DERIVED";
    case EvidenceProvenance::Estimated: return "ESTIMATED";
    case EvidenceProvenance::Synthetic: return "SYNTHETIC";
    case EvidenceProvenance::Reconstructed: return "RECONSTRUCTED";
  }
  return "UNKNOWN";
}

std::optional<EvidenceProvenance> evidence_provenance_from_string(std::string_view text) noexcept {
  for (const EnumName<EvidenceProvenance>& name : kProvenanceNames) {
    if (text == name.canonical || text == name.title_case) {
      return name.value;
    }
  }
  return std::nullopt;
}

std::string_view to_string(Freshness value) noexcept {
  switch (value) {
    case Freshness::Unknown: return "UNKNOWN";
    case Freshness::Fresh: return "FRESH";
    case Freshness::Stale: return "STALE";
    case Freshness::RevalidationRequired: return "REVALIDATION_REQUIRED";
  }
  return "UNKNOWN";
}

std::optional<Freshness> freshness_from_string(std::string_view text) noexcept {
  for (const EnumName<Freshness>& name : kFreshnessNames) {
    if (text == name.canonical || text == name.title_case) {
      return name.value;
    }
  }
  return std::nullopt;
}

std::string_view to_string(Durability value) noexcept {
  switch (value) {
    case Durability::Durable: return "DURABLE";
    case Durability::Ephemeral: return "EPHEMERAL";
  }
  return "UNKNOWN";
}

std::optional<Durability> durability_from_string(std::string_view text) noexcept {
  if (text == "DURABLE" || text == "Durable") {
    return Durability::Durable;
  }
  if (text == "EPHEMERAL" || text == "Ephemeral") {
    return Durability::Ephemeral;
  }
  return std::nullopt;
}

Freshness classify_freshness(Timestamp observed_at, Timestamp now, std::int64_t ttl_millis) noexcept {
  if (!observed_at.known()) {
    return Freshness::Unknown;
  }
  if (ttl_millis <= 0) {
    // No expiry was declared: a known observation does not age out.
    return Freshness::Fresh;
  }

  const std::int64_t observed = observed_at.millis();
  const std::int64_t current = now.millis();
  if (current < observed) {
    // Clock skew between the observer and this process is tolerated: a
    // future-dated observation is never reported as stale.
    return Freshness::Fresh;
  }

  // Both instants are known (positive) and current >= observed, so the
  // subtraction cannot overflow.
  const std::int64_t elapsed = current - observed;
  return elapsed <= ttl_millis ? Freshness::Fresh : Freshness::Stale;
}

const Clock& default_clock() noexcept {
  static const SystemClock clock_instance{};
  return clock_instance;
}

}  // namespace cluster_fabric
