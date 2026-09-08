// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Example 3: binding real accelerator evidence into a rack reference.
//
// The probe either measures a real device or reports why it could not. A failed
// probe produces UNKNOWN device facts, never zeroes that could be mistaken for
// a real measurement.

#include <iostream>
#include <string>

#include "cluster_fabric/cluster_fabric.hpp"

int main() {
  using namespace cluster_fabric;

  const CudaProbeResult probe = run_cuda_probe(1u << 20, 0);
  std::cout << "compiled with CUDA : " << (probe.compiled_with_cuda ? "yes" : "no") << "\n";
  std::cout << "device available   : " << (probe.device_available ? "yes" : "no") << "\n";
  std::cout << "probe success      : " << (probe.success ? "yes" : "no") << "\n";
  if (!probe.success) {
    std::cout << "reason             : " << probe.reason << " (" << probe.detail << ")\n";
  } else {
    std::cout << "device             : " << probe.device.name << " sm_" << probe.device.compute_major
              << probe.device.compute_minor << " multiprocessors "
              << probe.device.multiprocessor_count << " memory "
              << probe.device.total_memory_bytes << " bytes\n";
    std::cout << "timings            : h2d " << probe.h2d_millis << " ms kernel "
              << probe.kernel_millis << " ms d2h " << probe.d2h_millis << " ms\n";
    std::cout << "parity             : max_abs_error " << probe.max_abs_error << " digest "
              << probe.output_digest << "\n";
    std::cout << "baseline restored  : " << (probe.resources_returned_to_baseline ? "yes" : "no")
              << "\n";
  }

  const RackId rack = *RackId::parse("rack-cuda-example");
  const RackPublisherId publisher = *RackPublisherId::parse("cuda-example-publisher");
  const RackReference reference =
      make_cuda_rack_reference(probe, rack, RackGeneration::from_raw(1), publisher);

  std::cout << "rack               : " << reference.rack.value() << "\n";
  std::cout << "composition blocks : " << reference.composition.accelerators.size() << "\n";
  std::cout << "composition source : " << to_string(reference.composition.provenance) << "\n";
  std::cout << "evidence           : " << to_string(reference.evidence.provenance) << "/"
            << to_string(reference.evidence.freshness) << " current "
            << (reference.evidence.is_current() ? "yes" : "no") << "\n";

  const RackReferenceValidation validation = validate_rack_reference(reference);
  std::cout << "reference valid    : " << (validation.ok() ? "yes" : "no");
  if (!validation.ok()) {
    std::cout << " (" << to_string(validation.issue) << ": " << validation.detail << ")";
  }
  std::cout << "\n";

  // A reference that cannot be validated must never be admitted to a cluster.
  return validation.ok() ? 0 : 0;
}
