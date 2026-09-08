# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Optional CUDA evidence probe.
#
# Exactly one definition of cluster_fabric::run_cuda_probe exists in every
# build:
#   CUDA build     -> src/cuda/cuda_probe.cu (nvcc) and src/cuda_probe.cpp with
#                     CLUSTER_FABRIC_HAS_CUDA defined, which preprocesses the
#                     fallback definition out.
#   non-CUDA build -> src/cuda_probe.cpp only, providing the fallback.
#
# The core library never depends on CUDA. When the probe is available it is a
# separate component that consumers link explicitly, so a consumer without a
# CUDA toolkit still builds and links the core.

find_package(CUDAToolkit QUIET)

if(NOT CUDAToolkit_FOUND)
  message(STATUS "CUDA toolkit not found: the evidence probe reports cuda_not_compiled")
  return()
endif()

enable_language(CUDA)

if(CLUSTER_FABRIC_CUDA_ARCHITECTURES STREQUAL "native" AND CMAKE_VERSION VERSION_LESS 3.24)
  set(CLUSTER_FABRIC_CUDA_ARCHITECTURES "75;80;90")
  message(STATUS "CMake ${CMAKE_VERSION} cannot detect 'native' CUDA architectures; using ${CLUSTER_FABRIC_CUDA_ARCHITECTURES}")
endif()

add_library(cluster_fabric_cuda STATIC
  src/cuda/cuda_probe.cu
  src/cuda_probe.cpp)

add_library(cluster_fabric::cluster_fabric_cuda ALIAS cluster_fabric_cuda)

# Without this the always-compiled fallback would define run_cuda_probe too
# and the link would fail with LNK2005.
target_compile_definitions(cluster_fabric_cuda PRIVATE CLUSTER_FABRIC_HAS_CUDA=1)

target_include_directories(cluster_fabric_cuda
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(cluster_fabric_cuda PUBLIC cxx_std_20)

set_target_properties(cluster_fabric_cuda PROPERTIES
  CUDA_STANDARD 20
  CUDA_STANDARD_REQUIRED ON
  CUDA_SEPARABLE_COMPILATION OFF
  CUDA_ARCHITECTURES "${CLUSTER_FABRIC_CUDA_ARCHITECTURES}"
  CUDA_RESOLVE_DEVICE_SYMBOLS ON
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  OUTPUT_NAME cluster_fabric_cuda)

target_compile_options(cluster_fabric_cuda PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:/W4 /WX /permissive- /EHsc /utf-8 /Zc:__cplusplus>
  $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W4,/WX,/permissive-,/EHsc,/utf-8,/Zc:__cplusplus>)

target_link_libraries(cluster_fabric_cuda PUBLIC CUDA::cudart)

install(TARGETS cluster_fabric_cuda
  EXPORT cluster_fabricTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# This file is included from the top level, so a plain set() is what the
# parent (top-level) scope sees; PARENT_SCOPE would be a no-op here.
set(CLUSTER_FABRIC_CUDA_ENABLED 1)
message(STATUS "CUDA evidence probe enabled (architectures: ${CLUSTER_FABRIC_CUDA_ARCHITECTURES})")
