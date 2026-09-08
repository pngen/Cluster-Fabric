// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Real CUDA evidence probe.
//
// When this build has CUDA support the tests run the real probe on device 0 and
// require a measured device, a verified parity result, a restored memory
// baseline and a deterministic output digest. When CUDA is not compiled the
// tests require the probe to report UNSUPPORTED instead of inventing a result.

#include "harness.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

#include "cluster_fabric/cluster_fabric.hpp"

using namespace cluster_fabric;

namespace {

constexpr std::size_t kDefaultElements = 1u << 20;
constexpr std::size_t kMaxProbeElements = 1u << 26;

/// Prints every measured field so a failure can be diagnosed without a debugger.
void print_probe(const char* label, const CudaProbeResult& probe) {
  std::cout << "[" << label << "]\n";
  std::cout << "  compiled_with_cuda : " << (probe.compiled_with_cuda ? "true" : "false") << "\n";
  std::cout << "  device_available   : " << (probe.device_available ? "true" : "false") << "\n";
  std::cout << "  success            : " << (probe.success ? "true" : "false") << "\n";
  std::cout << "  reason             : " << probe.reason << "\n";
  std::cout << "  detail             : " << probe.detail << "\n";
  std::cout << "  element_count      : " << probe.element_count << "\n";
  std::cout << "  device_index       : " << probe.device.device_index << "\n";
  std::cout << "  device_name        : " << probe.device.name << "\n";
  std::cout << "  device_uuid        : " << probe.device.uuid << "\n";
  std::cout << "  compute_capability : " << probe.device.compute_major << "."
            << probe.device.compute_minor << "\n";
  std::cout << "  multiprocessors    : " << probe.device.multiprocessor_count << "\n";
  std::cout << "  total_memory_bytes : " << probe.device.total_memory_bytes << "\n";
  std::cout << "  driver_version     : " << probe.device.driver_version << "\n";
  std::cout << "  runtime_version    : " << probe.device.runtime_version << "\n";
  std::cout << "  clock_rate_khz     : " << probe.device.clock_rate_khz << "\n";
  std::cout << "  memory_bus_bits    : " << probe.device.memory_bus_width_bits << "\n";
  std::cout << "  h2d_millis         : " << probe.h2d_millis << "\n";
  std::cout << "  kernel_millis      : " << probe.kernel_millis << "\n";
  std::cout << "  d2h_millis         : " << probe.d2h_millis << "\n";
  std::cout << "  max_abs_error      : " << probe.max_abs_error << "\n";
  std::cout << "  output_digest      : " << probe.output_digest << "\n";
  std::cout << "  memory_before      : " << probe.device_memory_before_bytes << "\n";
  std::cout << "  memory_after       : " << probe.device_memory_after_bytes << "\n";
  std::cout << "  baseline_restored  : " << (probe.resources_returned_to_baseline ? "true" : "false")
            << "\n";
}

/// Fails with the measured reason and detail when a probe that must succeed did
/// not, so a failure is never reported as a bare boolean.
void expect_probe_success(const char* what, const CudaProbeResult& probe) {
  if (probe.success) {
    return;
  }
  print_probe(what, probe);
  CF_FAIL(std::string(what) + " did not succeed: reason=" + probe.reason +
          " detail=" + probe.detail);
}

[[nodiscard]] RackId probe_rack() { return *RackId::parse("rack-cuda-probe"); }

[[nodiscard]] RackPublisherId probe_publisher() {
  return *RackPublisherId::parse("cuda-probe-publisher");
}

}  // namespace

#if defined(CLUSTER_FABRIC_TEST_HAS_CUDA)

CF_TEST(cuda_probe_measures_the_real_device) {
  const CudaProbeResult probe = run_cuda_probe(kDefaultElements, 0);
  print_probe("device 0", probe);
  expect_probe_success("real probe run on device 0", probe);

  CF_EXPECT(probe.compiled_with_cuda);
  CF_EXPECT(probe.device_available);
  CF_EXPECT(probe.reason.empty());
  CF_EXPECT(probe.detail.empty());
  CF_EXPECT_EQ(probe.element_count, kDefaultElements);

  CF_EXPECT(probe.device.known());
  CF_EXPECT_EQ(probe.device.device_index, 0);
  CF_EXPECT(!probe.device.name.empty());
  CF_EXPECT_EQ(probe.device.uuid.size(), std::size_t{32});
  CF_EXPECT(probe.device.compute_major >= 1);
  CF_EXPECT(probe.device.compute_minor >= 0);
  CF_EXPECT(probe.device.multiprocessor_count > 0);
  CF_EXPECT(probe.device.total_memory_bytes > 0);
  CF_EXPECT(probe.device.driver_version > 0);
  CF_EXPECT(probe.device.runtime_version > 0);

  CF_EXPECT(probe.h2d_millis >= 0.0);
  CF_EXPECT(probe.kernel_millis >= 0.0);
  CF_EXPECT(probe.d2h_millis >= 0.0);
  CF_EXPECT(std::isfinite(probe.max_abs_error));
  CF_EXPECT(probe.max_abs_error >= 0.0);
  CF_EXPECT(probe.max_abs_error <= 1.0e-3);
  CF_EXPECT_NE(probe.output_digest, std::uint64_t{0});

  CF_EXPECT(probe.resources_returned_to_baseline);
  CF_EXPECT_EQ(probe.device_memory_before_bytes, probe.device_memory_after_bytes);
}

CF_TEST(cuda_probe_output_is_deterministic) {
  const CudaProbeResult first = run_cuda_probe(kDefaultElements, 0);
  const CudaProbeResult second = run_cuda_probe(kDefaultElements, 0);
  expect_probe_success("first deterministic run", first);
  expect_probe_success("second deterministic run", second);

  if (first.output_digest != second.output_digest) {
    print_probe("first", first);
    print_probe("second", second);
    CF_FAIL("identical probe runs produced different output digests: " +
            std::to_string(first.output_digest) + " vs " + std::to_string(second.output_digest));
  }
  CF_EXPECT_EQ(first.element_count, second.element_count);
  CF_EXPECT_EQ(first.device.uuid, second.device.uuid);
  CF_EXPECT_EQ(first.device.name, second.device.name);
  CF_EXPECT_EQ(first.max_abs_error, second.max_abs_error);
  CF_EXPECT(second.resources_returned_to_baseline);
}

CF_TEST(cuda_probe_clamps_element_count) {
  const CudaProbeResult zero = run_cuda_probe(0, 0);
  print_probe("element_count 0", zero);
  expect_probe_success("zero-element probe run", zero);
  CF_EXPECT_EQ(zero.element_count, std::size_t{1});

  const CudaProbeResult at_cap = run_cuda_probe(kMaxProbeElements, 0);
  print_probe("element_count 2^26", at_cap);
  expect_probe_success("cap-element probe run", at_cap);
  CF_EXPECT_EQ(at_cap.element_count, kMaxProbeElements);

  const CudaProbeResult beyond =
      run_cuda_probe(std::numeric_limits<std::size_t>::max(), 0);
  print_probe("element_count SIZE_MAX", beyond);
  expect_probe_success("oversized probe run", beyond);
  CF_EXPECT(beyond.element_count < std::numeric_limits<std::size_t>::max());
  CF_EXPECT_EQ(beyond.element_count, at_cap.element_count);
  CF_EXPECT_EQ(beyond.output_digest, at_cap.output_digest);
  CF_EXPECT(beyond.resources_returned_to_baseline);
}

CF_TEST(cuda_probe_rejects_out_of_range_device_index) {
  const int indices[] = {99, -1};
  for (int index : indices) {
    const CudaProbeResult probe = run_cuda_probe(1u << 12, index);
    print_probe("out-of-range device index", probe);
    CF_EXPECT(probe.compiled_with_cuda);
    CF_EXPECT(!probe.device_available);
    CF_EXPECT(!probe.success);
    CF_EXPECT_EQ(probe.reason, std::string("no_device"));
    CF_EXPECT(!probe.detail.empty());
    CF_EXPECT(!probe.device.known());
    CF_EXPECT_EQ(probe.device.multiprocessor_count, 0);
    CF_EXPECT_EQ(probe.device.total_memory_bytes, std::uint64_t{0});
    CF_EXPECT_EQ(probe.output_digest, std::uint64_t{0});
    CF_EXPECT(!probe.resources_returned_to_baseline);
    CF_EXPECT_EQ(probe.element_count, std::size_t{1u << 12});
  }
}

CF_TEST(cuda_rack_reference_is_measured_and_valid) {
  const CudaProbeResult probe = run_cuda_probe(kDefaultElements, 0);
  expect_probe_success("probe run for the rack reference", probe);

  const RackId rack = probe_rack();
  const RackPublisherId publisher = probe_publisher();
  const RackGeneration generation = RackGeneration::from_raw(7);
  const RackReference reference = make_cuda_rack_reference(probe, rack, generation, publisher);

  const RackReferenceValidation validation = validate_rack_reference(reference);
  if (!validation.ok()) {
    CF_FAIL("measured rack reference failed validation: issue=" +
            std::string(to_string(validation.issue)) + " subject=" + validation.subject +
            " detail=" + validation.detail);
  }
  CF_EXPECT_EQ(reference.rack, rack);
  CF_EXPECT_EQ(reference.generation, generation);
  CF_EXPECT(reference.publisher.has_value());
  CF_EXPECT_EQ(*reference.publisher, publisher);
  CF_EXPECT_EQ(reference.composition.provenance, EvidenceProvenance::Measured);
  CF_EXPECT_EQ(reference.evidence.provenance, EvidenceProvenance::Measured);
  CF_EXPECT(reference.evidence.is_current());
  CF_EXPECT_EQ(reference.composition.accelerators.size(), std::size_t{1});
  CF_EXPECT_EQ(reference.endpoints.size(), std::size_t{1});
  CF_EXPECT_EQ(reference.failure_domain_hints.size(), std::size_t{1});
  CF_EXPECT(!reference.origin_label.empty());
  CF_EXPECT_EQ(reference.rack_lifecycle, RackLifecycleState::Ready);

  if (reference.composition.accelerators.size() == 1) {
    const AcceleratorClassSummary& accelerator = reference.composition.accelerators.front();
    CF_EXPECT_EQ(accelerator.vendor, AcceleratorVendor::Nvidia);
    CF_EXPECT_EQ(accelerator.family, probe.device.name);
    CF_EXPECT(accelerator.device_count.has_value());
    CF_EXPECT_EQ(accelerator.device_count.value_or(0u), 1u);
    CF_EXPECT(accelerator.device_memory_bytes.has_value());
    CF_EXPECT_EQ(accelerator.device_memory_bytes.value_or(0ull), probe.device.total_memory_bytes);
  }
}

#else

CF_TEST(cuda_probe_fallback_reports_unsupported) {
  const CudaProbeResult probe = run_cuda_probe(kDefaultElements, 0);
  print_probe("fallback build", probe);

  CF_EXPECT(!probe.compiled_with_cuda);
  CF_EXPECT(!probe.device_available);
  CF_EXPECT(!probe.success);
  CF_EXPECT_EQ(probe.reason, std::string("cuda_not_compiled"));
  CF_EXPECT(!probe.detail.empty());
  CF_EXPECT_EQ(probe.element_count, kDefaultElements);
  CF_EXPECT(!probe.device.known());
  CF_EXPECT(probe.device.name.empty());
  CF_EXPECT(probe.device.uuid.empty());
  CF_EXPECT_EQ(probe.device.multiprocessor_count, 0);
  CF_EXPECT_EQ(probe.device.total_memory_bytes, std::uint64_t{0});
  CF_EXPECT_EQ(probe.output_digest, std::uint64_t{0});
  CF_EXPECT(!probe.resources_returned_to_baseline);
  CF_EXPECT_EQ(probe.device_memory_before_bytes, std::uint64_t{0});
  CF_EXPECT_EQ(probe.device_memory_after_bytes, std::uint64_t{0});

  // The fallback clamps identically: zero becomes one, absurd becomes the cap.
  CF_EXPECT_EQ(run_cuda_probe(0, 0).element_count, std::size_t{1});
  CF_EXPECT_EQ(run_cuda_probe(std::numeric_limits<std::size_t>::max(), 0).element_count,
               kMaxProbeElements);
}

CF_TEST(cuda_rack_reference_from_failed_probe_is_unknown) {
  const CudaProbeResult probe = run_cuda_probe(kDefaultElements, 0);
  CF_EXPECT(!probe.success);

  const RackReference reference = make_cuda_rack_reference(
      probe, probe_rack(), RackGeneration::from_raw(1), probe_publisher());

  CF_EXPECT_EQ(reference.composition.provenance, EvidenceProvenance::Unknown);
  CF_EXPECT_EQ(reference.evidence.provenance, EvidenceProvenance::Unknown);
  CF_EXPECT(!reference.evidence.is_current());
  CF_EXPECT(reference.composition.accelerators.empty());
  CF_EXPECT_EQ(reference.composition.provenance, EvidenceProvenance::Unknown);

  const RackReferenceValidation validation = validate_rack_reference(reference);
  CF_EXPECT(!validation.ok());
  CF_EXPECT_EQ(validation.issue, RackReferenceIssue::UnknownProvenance);
}

#endif

int main() { return cf_test::run("test_cuda_probe"); }
