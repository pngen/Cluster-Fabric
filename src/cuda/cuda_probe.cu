// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Real CUDA evidence probe.
//
// This translation unit is the only definition of run_cuda_probe in a CUDA
// build. It discovers a real device, allocates real device memory, uploads a
// deterministically filled host buffer, runs a real kernel, synchronizes,
// downloads the result, verifies it against a CPU parity computation, frees
// every allocation and reports the device memory in use before and after.
//
// No exception crosses the public boundary, CUDA errors are handled by return
// code only, and cudaDeviceReset is never called: the device may be shared
// with other processes. Only cudaFree releases what this probe allocated.

#ifndef CLUSTER_FABRIC_HAS_CUDA
#define CLUSTER_FABRIC_HAS_CUDA 1
#endif

#include <cuda_runtime.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "cluster_fabric/cuda_probe.hpp"

namespace cluster_fabric {

namespace {

/// Element bounds. Zero and absurd values are clamped, never rejected.
constexpr std::size_t kMinElementCount = 1;
constexpr std::size_t kMaxElementCount = 1u << 26;  // 64 Mi floats = 256 MiB
constexpr unsigned int kThreadsPerBlock = 256;

/// The transform both the kernel and the CPU parity check compute.
constexpr float kTransformScale = 1.5f;
constexpr float kTransformBias = -0.25f;
/// Device output must agree with the CPU reference to this relative bound.
constexpr double kParityTolerance = 1e-4;

/// FNV-1a 64 parameters, used for the deterministic output digest.
constexpr std::uint64_t kFnv1aOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnv1aPrime = 1099511628211ull;

/// y[i] = scale * x[i] + bias. A real device transform, not a memset.
__global__ void affine_transform_kernel(const float* __restrict__ input, float* __restrict__ output,
                                        std::size_t count, float scale, float bias) {
  const std::size_t index = static_cast<std::size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index < count) {
    output[index] = scale * input[index] + bias;
  }
}

/// Owns the device buffers and the timing events of one probe run. The
/// destructor is a safety net: the probe releases explicitly so that a failing
/// cudaFree can be reported as a failure instead of being swallowed.
class DeviceResources {
 public:
  DeviceResources() noexcept = default;
  DeviceResources(const DeviceResources&) = delete;
  DeviceResources& operator=(const DeviceResources&) = delete;
  ~DeviceResources() {
    release_events();
    (void)cudaFree(output_);
    (void)cudaFree(input_);
  }

  [[nodiscard]] float* input() const noexcept { return input_; }
  [[nodiscard]] float* output() const noexcept { return output_; }
  [[nodiscard]] cudaEvent_t start_event() const noexcept { return start_; }
  [[nodiscard]] cudaEvent_t stop_event() const noexcept { return stop_; }

  /// Creates both timing events. Returns the first failing status.
  cudaError_t create_events() noexcept {
    cudaError_t status = cudaEventCreate(&start_);
    if (status == cudaSuccess) {
      status = cudaEventCreate(&stop_);
    }
    return status;
  }

  /// Destroys the timing events. Host-side resources only.
  void release_events() noexcept {
    if (stop_ != nullptr) {
      (void)cudaEventDestroy(stop_);
      stop_ = nullptr;
    }
    if (start_ != nullptr) {
      (void)cudaEventDestroy(start_);
      start_ = nullptr;
    }
  }

  cudaError_t allocate_input(std::size_t bytes) noexcept {
    return cudaMalloc(reinterpret_cast<void**>(&input_), bytes);
  }

  cudaError_t allocate_output(std::size_t bytes) noexcept {
    return cudaMalloc(reinterpret_cast<void**>(&output_), bytes);
  }

  /// Frees the input buffer and clears the handle only on success, so the
  /// destructor can still retry a failed release.
  cudaError_t release_input() noexcept {
    const cudaError_t status = cudaFree(input_);
    if (status == cudaSuccess) {
      input_ = nullptr;
    }
    return status;
  }

  cudaError_t release_output() noexcept {
    const cudaError_t status = cudaFree(output_);
    if (status == cudaSuccess) {
      output_ = nullptr;
    }
    return status;
  }

 private:
  float* input_ = nullptr;
  float* output_ = nullptr;
  cudaEvent_t start_ = nullptr;
  cudaEvent_t stop_ = nullptr;
};

[[nodiscard]] std::size_t clamp_element_count(std::size_t requested) noexcept {
  if (requested < kMinElementCount) {
    return kMinElementCount;
  }
  if (requested > kMaxElementCount) {
    return kMaxElementCount;
  }
  return requested;
}

[[nodiscard]] std::string describe_cuda_error(cudaError_t status) {
  const char* name = cudaGetErrorName(status);
  const char* message = cudaGetErrorString(status);
  std::string text = (name != nullptr) ? name : "cudaErrorUnknown";
  text += ": ";
  text += (message != nullptr) ? message : "no description";
  return text;
}

/// Renders the 16 device UUID bytes as 32 lower-case hex characters.
[[nodiscard]] std::string hex_uuid(const cudaUUID_t& uuid) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(32);
  for (std::size_t i = 0; i < 16; ++i) {
    const unsigned int byte = static_cast<unsigned char>(uuid.bytes[i]);
    text.push_back(kHex[byte >> 4]);
    text.push_back(kHex[byte & 0x0Fu]);
  }
  return text;
}

/// Deterministic host input in [-0.5, 0.5). Integer mix, so the value does not
/// depend on the floating-point environment.
[[nodiscard]] float deterministic_input(std::size_t index) noexcept {
  const std::uint32_t mixed = static_cast<std::uint32_t>(index) * 2654435761u + 1013904223u;
  return static_cast<float>(mixed >> 8) / 16777216.0f - 0.5f;
}

/// The same transform, evaluated on the host in double precision.
[[nodiscard]] double cpu_reference(std::size_t index) noexcept {
  return static_cast<double>(kTransformScale) * static_cast<double>(deterministic_input(index)) +
         static_cast<double>(kTransformBias);
}

/// FNV-1a 64 over the raw byte image of the returned array.
[[nodiscard]] std::uint64_t fnv1a64(const float* data, std::size_t count) noexcept {
  std::uint64_t hash = kFnv1aOffsetBasis;
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
  const std::size_t byte_count = count * sizeof(float);
  for (std::size_t i = 0; i < byte_count; ++i) {
    hash ^= static_cast<std::uint64_t>(bytes[i]);
    hash *= kFnv1aPrime;
  }
  return hash;
}

[[nodiscard]] CudaProbeResult fail(CudaProbeResult result, const char* reason,
                                   std::string detail) {
  result.success = false;
  result.reason = reason;
  result.detail = std::move(detail);
  return result;
}

/// Waits for the stop event and converts it to milliseconds.
[[nodiscard]] cudaError_t time_section(cudaEvent_t start, cudaEvent_t stop,
                                       double& millis) noexcept {
  cudaError_t status = cudaEventSynchronize(stop);
  if (status != cudaSuccess) {
    return status;
  }
  float elapsed = 0.0f;
  status = cudaEventElapsedTime(&elapsed, start, stop);
  if (status != cudaSuccess) {
    return status;
  }
  millis = static_cast<double>(elapsed);
  return cudaSuccess;
}

[[nodiscard]] CudaProbeResult probe_impl(std::size_t element_count, int device_index) {
  CudaProbeResult result;
  result.compiled_with_cuda = true;
  const std::size_t count = clamp_element_count(element_count);
  result.element_count = count;

  int device_count = 0;
  cudaError_t status = cudaGetDeviceCount(&device_count);
  if (status != cudaSuccess) {
    return fail(std::move(result), "cuda_unavailable",
                "cudaGetDeviceCount failed: " + describe_cuda_error(status));
  }
  if (device_count <= 0) {
    return fail(std::move(result), "no_device", "no CUDA device is present on this host");
  }
  if (device_index < 0 || device_index >= device_count) {
    return fail(std::move(result), "no_device",
                "requested device index " + std::to_string(device_index) +
                    " is outside [0, " + std::to_string(device_count) + ")");
  }

  cudaDeviceProp properties{};
  status = cudaGetDeviceProperties(&properties, device_index);
  if (status != cudaSuccess) {
    return fail(std::move(result), "device_query_failed",
                "cudaGetDeviceProperties failed: " + describe_cuda_error(status));
  }

  status = cudaSetDevice(device_index);
  if (status != cudaSuccess) {
    return fail(std::move(result), "context_failed",
                "cudaSetDevice failed: " + describe_cuda_error(status));
  }
  // Materialize the primary context. This does not reset the device and does
  // not disturb other processes using it.
  status = cudaFree(nullptr);
  if (status != cudaSuccess) {
    return fail(std::move(result), "context_failed",
                "cudaFree(nullptr) could not materialize the primary context: " +
                    describe_cuda_error(status));
  }
  // From here on a usable device was found and its context exists.
  result.device_available = true;

  int driver_version = 0;
  int runtime_version = 0;
  status = cudaDriverGetVersion(&driver_version);
  if (status != cudaSuccess) {
    return fail(std::move(result), "device_query_failed",
                "cudaDriverGetVersion failed: " + describe_cuda_error(status));
  }
  status = cudaRuntimeGetVersion(&runtime_version);
  if (status != cudaSuccess) {
    return fail(std::move(result), "device_query_failed",
                "cudaRuntimeGetVersion failed: " + describe_cuda_error(status));
  }

  result.device.device_index = device_index;
  result.device.name = properties.name;
  result.device.compute_major = properties.major;
  result.device.compute_minor = properties.minor;
  result.device.multiprocessor_count = properties.multiProcessorCount;
  result.device.total_memory_bytes = static_cast<std::uint64_t>(properties.totalGlobalMem);
  result.device.driver_version = driver_version;
  result.device.runtime_version = runtime_version;
  result.device.uuid = hex_uuid(properties.uuid);

  // These two are read as attributes rather than from the deprecated
  // cudaDeviceProp fields. A failed query leaves the field at zero, which
  // means UNKNOWN and is never invented.
  int attribute = 0;
  if (cudaDeviceGetAttribute(&attribute, cudaDevAttrClockRate, device_index) == cudaSuccess) {
    result.device.clock_rate_khz = attribute;
  }
  if (cudaDeviceGetAttribute(&attribute, cudaDevAttrGlobalMemoryBusWidth, device_index) ==
      cudaSuccess) {
    result.device.memory_bus_width_bits = attribute;
  }

  DeviceResources resources;
  status = resources.create_events();
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "cudaEventCreate failed: " + describe_cuda_error(status));
  }

  // Warm up the context before the baseline measurement: load the module, JIT
  // the kernel and let the driver finish its lazy allocations, so the
  // before/after comparison reflects only this probe's own allocations.
  void* warmup = nullptr;
  status = cudaMalloc(&warmup, sizeof(float));
  if (status != cudaSuccess) {
    return fail(std::move(result), "alloc_failed",
                "warm-up cudaMalloc failed: " + describe_cuda_error(status));
  }
  affine_transform_kernel<<<1, 1>>>(static_cast<const float*>(warmup),
                                    static_cast<float*>(warmup), kMinElementCount,
                                    kTransformScale, kTransformBias);
  status = cudaGetLastError();
  if (status == cudaSuccess) {
    status = cudaDeviceSynchronize();
  }
  if (status != cudaSuccess) {
    (void)cudaFree(warmup);
    return fail(std::move(result), "kernel_failed",
                "warm-up kernel launch failed: " + describe_cuda_error(status));
  }
  status = cudaFree(warmup);
  if (status != cudaSuccess) {
    return fail(std::move(result), "free_failed",
                "warm-up cudaFree failed: " + describe_cuda_error(status));
  }

  std::size_t free_before = 0;
  std::size_t total_bytes = 0;
  status = cudaMemGetInfo(&free_before, &total_bytes);
  if (status != cudaSuccess) {
    return fail(std::move(result), "device_query_failed",
                "cudaMemGetInfo (before) failed: " + describe_cuda_error(status));
  }
  const std::size_t used_before = (total_bytes >= free_before) ? (total_bytes - free_before) : 0;
  result.device_memory_before_bytes = static_cast<std::uint64_t>(used_before);

  const std::size_t bytes = count * sizeof(float);
  std::vector<float> host_input(count);
  std::vector<float> host_output(count);
  for (std::size_t i = 0; i < count; ++i) {
    host_input[i] = deterministic_input(i);
  }

  status = resources.allocate_input(bytes);
  if (status != cudaSuccess) {
    return fail(std::move(result), "alloc_failed",
                "cudaMalloc (input) failed: " + describe_cuda_error(status));
  }
  status = resources.allocate_output(bytes);
  if (status != cudaSuccess) {
    return fail(std::move(result), "alloc_failed",
                "cudaMalloc (output) failed: " + describe_cuda_error(status));
  }

  status = cudaEventRecord(resources.start_event());
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "cudaEventRecord (h2d start) failed: " + describe_cuda_error(status));
  }
  status = cudaMemcpy(resources.input(), host_input.data(), bytes, cudaMemcpyHostToDevice);
  if (status != cudaSuccess) {
    return fail(std::move(result), "copy_failed",
                "host-to-device cudaMemcpy failed: " + describe_cuda_error(status));
  }
  status = cudaEventRecord(resources.stop_event());
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "cudaEventRecord (h2d stop) failed: " + describe_cuda_error(status));
  }
  status = time_section(resources.start_event(), resources.stop_event(), result.h2d_millis);
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "host-to-device timing failed: " + describe_cuda_error(status));
  }

  const std::size_t blocks = (count + kThreadsPerBlock - 1) / kThreadsPerBlock;
  const unsigned int grid = static_cast<unsigned int>(blocks);

  status = cudaEventRecord(resources.start_event());
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "cudaEventRecord (kernel start) failed: " + describe_cuda_error(status));
  }
  affine_transform_kernel<<<grid, kThreadsPerBlock>>>(resources.input(), resources.output(), count,
                                                      kTransformScale, kTransformBias);
  status = cudaGetLastError();
  if (status != cudaSuccess) {
    return fail(std::move(result), "kernel_failed",
                "kernel launch failed: " + describe_cuda_error(status));
  }
  status = cudaEventRecord(resources.stop_event());
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "cudaEventRecord (kernel stop) failed: " + describe_cuda_error(status));
  }
  status = cudaDeviceSynchronize();
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "cudaDeviceSynchronize failed: " + describe_cuda_error(status));
  }
  status = time_section(resources.start_event(), resources.stop_event(), result.kernel_millis);
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "kernel timing failed: " + describe_cuda_error(status));
  }

  status = cudaEventRecord(resources.start_event());
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "cudaEventRecord (d2h start) failed: " + describe_cuda_error(status));
  }
  status = cudaMemcpy(host_output.data(), resources.output(), bytes, cudaMemcpyDeviceToHost);
  if (status != cudaSuccess) {
    return fail(std::move(result), "copy_failed",
                "device-to-host cudaMemcpy failed: " + describe_cuda_error(status));
  }
  status = cudaEventRecord(resources.stop_event());
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "cudaEventRecord (d2h stop) failed: " + describe_cuda_error(status));
  }
  status = time_section(resources.start_event(), resources.stop_event(), result.d2h_millis);
  if (status != cudaSuccess) {
    return fail(std::move(result), "sync_failed",
                "device-to-host timing failed: " + describe_cuda_error(status));
  }

  result.output_digest = fnv1a64(host_output.data(), count);

  double max_abs_error = 0.0;
  double reference_scale = 1.0;
  bool finite = true;
  for (std::size_t i = 0; i < count; ++i) {
    const double expected = cpu_reference(i);
    const double actual = static_cast<double>(host_output[i]);
    if (!std::isfinite(actual)) {
      finite = false;
      break;
    }
    const double error = std::fabs(actual - expected);
    if (error > max_abs_error) {
      max_abs_error = error;
    }
    const double magnitude = std::fabs(expected);
    if (magnitude > reference_scale) {
      reference_scale = magnitude;
    }
  }
  result.max_abs_error = max_abs_error;

  status = resources.release_output();
  if (status != cudaSuccess) {
    return fail(std::move(result), "free_failed",
                "cudaFree (output) failed: " + describe_cuda_error(status));
  }
  status = resources.release_input();
  if (status != cudaSuccess) {
    return fail(std::move(result), "free_failed",
                "cudaFree (input) failed: " + describe_cuda_error(status));
  }
  resources.release_events();

  std::size_t free_after = 0;
  std::size_t total_after = 0;
  status = cudaMemGetInfo(&free_after, &total_after);
  if (status != cudaSuccess) {
    return fail(std::move(result), "device_query_failed",
                "cudaMemGetInfo (after) failed: " + describe_cuda_error(status));
  }
  const std::size_t used_after = (total_after >= free_after) ? (total_after - free_after) : 0;
  result.device_memory_after_bytes = static_cast<std::uint64_t>(used_after);
  // Exact equality: the baseline was measured after the context was fully
  // warm, so every byte that differs belongs to this probe.
  result.resources_returned_to_baseline = (used_after == used_before);

  if (!finite) {
    return fail(std::move(result), "verify_failed",
                "device output contained a non-finite value");
  }
  const double tolerance = kParityTolerance * reference_scale;
  if (!(max_abs_error <= tolerance)) {
    return fail(std::move(result), "verify_failed",
                "max_abs_error " + std::to_string(max_abs_error) +
                    " exceeds the parity tolerance " + std::to_string(tolerance));
  }
  if (!result.resources_returned_to_baseline) {
    return fail(std::move(result), "free_failed",
                "device memory in use after the probe (" + std::to_string(used_after) +
                    " bytes) differs from the baseline (" + std::to_string(used_before) +
                    " bytes)");
  }

  result.success = true;
  return result;
}

}  // namespace

CudaProbeResult run_cuda_probe(std::size_t element_count, int device_index) {
  try {
    return probe_impl(element_count, device_index);
  } catch (const std::exception& error) {
    CudaProbeResult result;
    result.compiled_with_cuda = true;
    result.element_count = clamp_element_count(element_count);
    result.reason = "alloc_failed";
    result.detail = std::string("host allocation failed inside the probe: ") + error.what();
    return result;
  } catch (...) {
    CudaProbeResult result;
    result.compiled_with_cuda = true;
    result.element_count = clamp_element_count(element_count);
    result.reason = "alloc_failed";
    result.detail = "host allocation failed inside the probe: unknown exception";
    return result;
  }
}

}  // namespace cluster_fabric
