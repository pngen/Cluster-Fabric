// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// CUDA-independent half of the probe.
//
// make_cuda_rack_reference needs no CUDA: it maps an already obtained
// CudaProbeResult onto the narrow rack contract. It therefore lives in this
// always-compiled translation unit, next to the fallback run_cuda_probe used
// when the build has no CUDA support.
//
// Split: a CUDA build compiles src/cuda/cuda_probe.cu (the real
// run_cuda_probe) and defines CLUSTER_FABRIC_HAS_CUDA for this file, so the
// fallback below is not compiled. A build without CUDA compiles only this
// file. Exactly one definition of run_cuda_probe exists in either case.

#include "cluster_fabric/cuda_probe.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cluster_fabric {

namespace {

constexpr char kOriginLabel[] = "cluster-fabric:cuda-probe";
constexpr char kEndpointId[] = "gpu0";
constexpr char kFallbackIdentity[] = "cuda-probe";
constexpr std::uint64_t kFirstPublication = 1;

static_assert(validate_identity(kEndpointId).status == IdentityStatus::Ok,
              "endpoint identity must be a valid identity");
static_assert(validate_identity(kFallbackIdentity).status == IdentityStatus::Ok,
              "fallback identity must be a valid identity");

/// A distinct process-incarnation label for the publication authored here.
[[nodiscard]] std::string make_boot_text(std::int64_t unix_millis) {
  static std::atomic<std::uint64_t> sequence{0};
  const std::uint64_t ordinal = sequence.fetch_add(1, std::memory_order_relaxed);
  return std::string(kFallbackIdentity) + "-" + std::to_string(unix_millis) + "-" +
         std::to_string(ordinal);
}

/// Parses a textual identity and falls back to a compile-time validated one.
/// The fallback text is validated by the static_assert above, so the second
/// parse cannot fail.
template <class Id>
[[nodiscard]] Id parse_id(std::string_view text) {
  const std::optional<Id> parsed = Id::parse(text);
  if (parsed.has_value()) {
    return *parsed;
  }
  return *Id::parse(kFallbackIdentity);
}

}  // namespace

RackReference make_cuda_rack_reference(const CudaProbeResult& probe, const RackId& rack,
                                       RackGeneration generation,
                                       const RackPublisherId& publisher) {
  const EvidenceProvenance provenance =
      probe.success ? EvidenceProvenance::Measured : EvidenceProvenance::Unknown;
  const EvidenceStamp stamp = probe.success
                                  ? EvidenceStamp::make(provenance, default_clock().now(), 0)
                                  : EvidenceStamp::unknown();

  RackCompositionSummary composition;
  composition.provenance = provenance;
  if (probe.success) {
    AcceleratorClassSummary accelerator;
    accelerator.vendor = AcceleratorVendor::Nvidia;
    accelerator.family = probe.device.name;
    accelerator.device_count = std::uint32_t{1};
    accelerator.device_memory_bytes = probe.device.total_memory_bytes;
    composition.accelerators.push_back(std::move(accelerator));
  }

  std::vector<RackEndpoint> endpoints;
  endpoints.push_back(RackEndpoint{parse_id<RackEndpointId>(kEndpointId), std::nullopt,
                                   std::nullopt,
                                   probe.success ? ConnectivityClass::DirectFabric
                                                 : ConnectivityClass::Unknown,
                                   stamp});

  std::vector<RackFailureDomainHint> hints;
  hints.push_back(RackFailureDomainHint{FailureDomainClass::Rack,
                                        parse_id<FailureDomainId>(rack.value()), stamp});

  RackReference reference{rack,
                          generation,
                          RackLifecycleState::Ready,
                          RackCurrentness::Current,
                          std::move(composition),
                          std::move(endpoints),
                          std::move(hints),
                          stamp,
                          publisher,
                          PublicationGeneration::from_raw(kFirstPublication),
                          parse_id<RackAgentBootId>(make_boot_text(stamp.observed_at.millis())),
                          ClusterEpoch{},
                          CoordinatorEpoch{},
                          HealthState::Unknown,
                          std::string(kOriginLabel)};
  return reference;
}

#if !defined(CLUSTER_FABRIC_HAS_CUDA)

namespace {

/// The element bounds of the probe, mirrored from the CUDA implementation so
/// that a build without CUDA reports the same bounded count. Zero and absurd
/// values are clamped, never rejected.
constexpr std::size_t kMinElementCount = 1;
constexpr std::size_t kMaxElementCount = 1u << 26;

[[nodiscard]] std::size_t clamp_element_count(std::size_t requested) noexcept {
  if (requested < kMinElementCount) {
    return kMinElementCount;
  }
  if (requested > kMaxElementCount) {
    return kMaxElementCount;
  }
  return requested;
}

}  // namespace

CudaProbeResult run_cuda_probe(std::size_t element_count, int /*device_index*/) {
  CudaProbeResult result;
  result.compiled_with_cuda = false;
  result.device_available = false;
  result.success = false;
  result.reason = "cuda_not_compiled";
  result.detail = "this build was compiled without CUDA support";
  result.element_count = clamp_element_count(element_count);
  return result;
}

#endif  // !defined(CLUSTER_FABRIC_HAS_CUDA)

}  // namespace cluster_fabric
