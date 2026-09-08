// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Real CUDA evidence probe.
//
// This probe exists to prove that a cluster composition can bind real
// lower-level infrastructure evidence: an actual device is discovered, real
// memory is allocated, a real kernel runs, results are verified against a CPU
// parity computation, and resources are returned to baseline. Cluster Fabric
// is not a GPU runtime and does not schedule the device.
//
// When CUDA is not present at build time the probe reports UNSUPPORTED rather
// than inventing a result.

#ifndef CLUSTER_FABRIC_CUDA_PROBE_HPP
#define CLUSTER_FABRIC_CUDA_PROBE_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "cluster_fabric/generation.hpp"
#include "cluster_fabric/identity.hpp"
#include "cluster_fabric/rack_reference.hpp"

namespace cluster_fabric {

struct CudaDeviceInfo {
  int device_index = -1;
  std::string name;
  int compute_major = 0;
  int compute_minor = 0;
  int multiprocessor_count = 0;
  std::uint64_t total_memory_bytes = 0;
  int driver_version = 0;
  int runtime_version = 0;
  std::string uuid;
  int clock_rate_khz = 0;
  int memory_bus_width_bits = 0;

  [[nodiscard]] bool known() const noexcept { return device_index >= 0; }
};

struct CudaProbeResult {
  /// True when this build was compiled with CUDA support.
  bool compiled_with_cuda = false;
  /// True when a usable CUDA device was found at runtime.
  bool device_available = false;
  /// True when every step of the probe completed and verified.
  bool success = false;
  CudaDeviceInfo device;
  /// Stable reason code when the probe did not succeed.
  std::string reason;
  std::string detail;

  std::size_t element_count = 0;
  double h2d_millis = 0.0;
  double kernel_millis = 0.0;
  double d2h_millis = 0.0;
  double max_abs_error = 0.0;
  /// Device memory in use before and after the probe, in bytes.
  std::uint64_t device_memory_before_bytes = 0;
  std::uint64_t device_memory_after_bytes = 0;
  bool resources_returned_to_baseline = false;
  /// Deterministic digest of the verified output, for reproducibility reports.
  std::uint64_t output_digest = 0;
};

/// Runs the probe on device p device_index. p element_count is bounded
/// internally.
[[nodiscard]] CudaProbeResult run_cuda_probe(std::size_t element_count = 1u << 20,
                                             int device_index = 0);

/// Builds a narrow rack reference describing the probed accelerator as one
/// SYNTHETIC rack whose composition evidence is REAL.
[[nodiscard]] RackReference make_cuda_rack_reference(const CudaProbeResult& probe,
                                                     const RackId& rack,
                                                     RackGeneration generation,
                                                     const RackPublisherId& publisher);

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_CUDA_PROBE_HPP
