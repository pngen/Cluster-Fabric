// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Evidence classification, freshness and provenance.
//
// Cluster Fabric never collapses provenance into a single "valid" boolean.
// Every observation records how it was obtained, when, and how long that claim
// remains defensible. Absence of evidence is not positive evidence: the
// default provenance of every field is UNKNOWN.

#ifndef CLUSTER_FABRIC_EVIDENCE_HPP
#define CLUSTER_FABRIC_EVIDENCE_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "cluster_fabric/identity.hpp"

namespace cluster_fabric {

/// How an observation was obtained.
///
///   MEASURED      - read directly from the device or platform at the moment
///                   of publication by the process that published it.
///   REPORTED      - asserted by another runtime or by an operator; Cluster
///                   Fabric relays the claim without having observed it.
///   DERIVED       - computed by Cluster Fabric from other evidence that is
///                   itself classified (for example an aggregate over
///                   homogeneous member quantities).
///   ESTIMATED     - inferred by a model or interpolation; no measurement
///                   backs the value.
///   SYNTHETIC     - produced by the deterministic synthetic cluster
///                   laboratory. Never a physical fact.
///   RECONSTRUCTED - recovered from persisted durable state after a restart.
///                   It is a memory of a past declaration, not a current
///                   observation, and never satisfies a currentness check.
///   UNKNOWN       - nothing is known.
enum class EvidenceProvenance : std::uint8_t {
  Unknown = 0,
  Measured = 1,
  Reported = 2,
  Derived = 3,
  Estimated = 4,
  Synthetic = 5,
  Reconstructed = 6,
};

[[nodiscard]] std::string_view to_string(EvidenceProvenance value) noexcept;
[[nodiscard]] std::optional<EvidenceProvenance> evidence_provenance_from_string(
    std::string_view text) noexcept;

/// True only for provenance classes that represent an observation or a
/// directly asserted external claim made by a live authority.
[[nodiscard]] constexpr bool is_physical_provenance(EvidenceProvenance value) noexcept {
  return value == EvidenceProvenance::Measured || value == EvidenceProvenance::Reported ||
         value == EvidenceProvenance::Derived;
}

/// True when the provenance may be used to satisfy a "current evidence"
/// requirement. SYNTHETIC, ESTIMATED, RECONSTRUCTED and UNKNOWN may not.
[[nodiscard]] constexpr bool is_current_evidence_provenance(EvidenceProvenance value) noexcept {
  return value == EvidenceProvenance::Measured || value == EvidenceProvenance::Reported;
}

/// Freshness of an observation relative to the observing clock.
enum class Freshness : std::uint8_t {
  /// No timestamp, or no evidence at all.
  Unknown = 0,
  /// Within the declared time-to-live.
  Fresh = 1,
  /// Past the declared time-to-live. The observation happened, but it is no
  /// longer a statement about the present.
  Stale = 2,
  /// The authority that produced the observation is gone (process death) or
  /// the coordinator was restarted. The observation must be republished under
  /// fresh authority before it can be considered current.
  RevalidationRequired = 3,
};

[[nodiscard]] std::string_view to_string(Freshness value) noexcept;
[[nodiscard]] std::optional<Freshness> freshness_from_string(std::string_view text) noexcept;

/// Whether a fact survives a coordinator restart.
enum class Durability : std::uint8_t {
  /// A declaration about the infrastructure that remains true until it is
  /// explicitly superseded: identities, membership, declared domains,
  /// retired identities, format metadata.
  Durable = 0,
  /// A live observation that is only true while the process that made it is
  /// alive and within its time-to-live: process authority, reachability,
  /// health, currentness, measured latency or bandwidth.
  Ephemeral = 1,
};

[[nodiscard]] std::string_view to_string(Durability value) noexcept;
[[nodiscard]] std::optional<Durability> durability_from_string(std::string_view text) noexcept;

/// Wall-clock instant in milliseconds since the Unix epoch. Zero means
/// unknown; it is never treated as a valid observation time.
struct Timestamp {
  std::int64_t unix_millis = 0;

  [[nodiscard]] static constexpr Timestamp unknown() noexcept { return Timestamp{}; }
  [[nodiscard]] static constexpr Timestamp from_unix_millis(std::int64_t millis) noexcept {
    return Timestamp{millis};
  }
  [[nodiscard]] constexpr bool known() const noexcept { return unix_millis > 0; }
  [[nodiscard]] constexpr std::int64_t millis() const noexcept { return unix_millis; }

  friend constexpr bool operator==(Timestamp lhs, Timestamp rhs) noexcept = default;
  friend constexpr auto operator<=>(Timestamp lhs, Timestamp rhs) noexcept = default;
};

/// Source of timestamps. Injectable so tests are deterministic.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = default;
  Clock(Clock&&) = default;
  Clock& operator=(const Clock&) = default;
  Clock& operator=(Clock&&) = default;
  virtual ~Clock() = default;

  [[nodiscard]] virtual Timestamp now() const = 0;
};

/// The process default clock, backed by the system wall clock.
[[nodiscard]] const Clock& default_clock() noexcept;

/// Test clock with a manually advanced instant.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::int64_t start_unix_millis = 1'000'000) noexcept
      : now_(start_unix_millis) {}

  [[nodiscard]] Timestamp now() const override { return Timestamp::from_unix_millis(now_); }

  void advance_millis(std::int64_t delta) noexcept {
    if (delta > 0) {
      now_ += delta;
    }
  }

  void set_unix_millis(std::int64_t value) noexcept { now_ = value; }

 private:
  std::int64_t now_ = 0;
};

/// Classifies freshness of a stamp against p now given a time-to-live.
/// A zero time-to-live means "no expiry declared", which yields Fresh for any
/// known timestamp and Unknown for an unknown one.
[[nodiscard]] Freshness classify_freshness(Timestamp observed_at, Timestamp now,
                                           std::int64_t ttl_millis) noexcept;

/// A complete evidence stamp. This is the unit that mutation authority checks
/// and that snapshots bind.
struct EvidenceStamp {
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  Timestamp observed_at = Timestamp::unknown();
  Freshness freshness = Freshness::Unknown;
  std::int64_t ttl_millis = 0;

  [[nodiscard]] static EvidenceStamp unknown() noexcept { return EvidenceStamp{}; }

  [[nodiscard]] static EvidenceStamp make(EvidenceProvenance provenance, Timestamp observed_at,
                                          std::int64_t ttl_millis) noexcept {
    EvidenceStamp stamp;
    stamp.provenance = provenance;
    stamp.observed_at = observed_at;
    stamp.ttl_millis = ttl_millis;
    stamp.freshness = classify_freshness(observed_at, observed_at, ttl_millis);
    return stamp;
  }

  [[nodiscard]] bool is_unknown() const noexcept {
    return provenance == EvidenceProvenance::Unknown;
  }

  /// True when this stamp may back a current statement: known provenance that
  /// is eligible, a known timestamp, and FRESH.
  [[nodiscard]] bool is_current() const noexcept {
    return is_current_evidence_provenance(provenance) && observed_at.known() &&
           freshness == Freshness::Fresh;
  }

  [[nodiscard]] bool requires_revalidation() const noexcept {
    return freshness == Freshness::RevalidationRequired ||
           provenance == EvidenceProvenance::Reconstructed;
  }

  /// Re-evaluates freshness against p now. Revalidation-required stamps are
  /// never downgraded by time alone.
  void refresh(Timestamp now) noexcept {
    if (freshness == Freshness::RevalidationRequired) {
      return;
    }
    freshness = classify_freshness(observed_at, now, ttl_millis);
  }

  /// Marks the stamp as requiring revalidation, preserving the original
  /// observation record. Recovered or orphaned evidence uses this.
  void mark_revalidation_required() noexcept { freshness = Freshness::RevalidationRequired; }

  friend bool operator==(const EvidenceStamp&, const EvidenceStamp&) = default;
};

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_EVIDENCE_HPP
