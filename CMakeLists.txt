# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Cluster Fabric: vendor-neutral cluster-level infrastructure runtime.
#
# The core library builds and installs standalone with no third-party
# dependencies. CUDA is an optional, isolated evidence-probe component.

cmake_minimum_required(VERSION 3.20)

project(ClusterFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral cluster-level infrastructure runtime"
  HOMEPAGE_URL "https://github.com/pngen/Cluster-Fabric"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(CLUSTER_FABRIC_BUILD_TESTS "Build the Cluster Fabric test suite" ON)
option(CLUSTER_FABRIC_BUILD_EXAMPLES "Build the Cluster Fabric examples" ON)
option(CLUSTER_FABRIC_BUILD_BENCHMARKS "Build the Cluster Fabric benchmarks" ON)
option(CLUSTER_FABRIC_ENABLE_CUDA "Build the optional CUDA evidence probe when a CUDA toolkit is present" ON)
option(CLUSTER_FABRIC_ENABLE_SANITIZERS "Build with address and undefined-behaviour sanitizers when the toolchain supports them" OFF)
option(CLUSTER_FABRIC_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)

set(CLUSTER_FABRIC_CUDA_ARCHITECTURES "native" CACHE STRING
  "CUDA architectures for the evidence probe. 'native' detects the local GPU at configure time; set an explicit list such as '90;120' on machines without a visible GPU.")

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Every first-party target is compiled with the same strict, explicit warning
# set. No warning is ever globally suppressed.
function(cluster_fabric_strict_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
    if(CLUSTER_FABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
      -Wsign-conversion -Wold-style-cast -Wcast-qual -Wundef)
    if(CLUSTER_FABRIC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(CLUSTER_FABRIC_ENABLE_SANITIZERS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------

set(CLUSTER_FABRIC_SOURCES
  src/version.cpp
  src/identity.cpp
  src/generation.cpp
  src/evidence.cpp
  src/taxonomy.cpp
  src/error.cpp
  src/explanation.cpp
  src/lifecycle.cpp
  src/rack_reference.cpp
  src/topology.cpp
  src/cluster_state.cpp
  src/invariant.cpp
  src/snapshot.cpp
  src/mutation.cpp
  src/persistence.cpp
  src/protocol.cpp
  src/coordinator.cpp
  src/coordinator_engine.cpp
  src/coordinator_network.cpp
  src/rack_agent.cpp
  src/synthetic.cpp)

# The CUDA-independent fallback definition of run_cuda_probe is part of the
# core only when this build does not provide the real probe. A static linker
# resolves the first definition it sees, so a build that linked both would
# silently report cuda_not_compiled on a machine with a working GPU.
set(CLUSTER_FABRIC_CUDA_ENABLED 0)
if(CLUSTER_FABRIC_ENABLE_CUDA)
  find_package(CUDAToolkit QUIET)
  if(CUDAToolkit_FOUND)
    set(CLUSTER_FABRIC_CUDA_ENABLED 1)
  endif()
endif()

set(CLUSTER_FABRIC_CORE_EXTRA_SOURCES "")
if(NOT CLUSTER_FABRIC_CUDA_ENABLED)
  list(APPEND CLUSTER_FABRIC_CORE_EXTRA_SOURCES src/cuda_probe.cpp)
endif()

add_library(cluster_fabric
  ${CLUSTER_FABRIC_SOURCES}
  ${CLUSTER_FABRIC_CORE_EXTRA_SOURCES})

add_library(cluster_fabric::cluster_fabric ALIAS cluster_fabric)

target_include_directories(cluster_fabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(cluster_fabric PUBLIC cxx_std_20)

set_target_properties(cluster_fabric PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  WINDOWS_EXPORT_ALL_SYMBOLS ON
  OUTPUT_NAME cluster_fabric)

cluster_fabric_strict_options(cluster_fabric)

if(WIN32)
  target_link_libraries(cluster_fabric PRIVATE ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(cluster_fabric PRIVATE Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# Optional CUDA evidence probe
# ---------------------------------------------------------------------------

if(CLUSTER_FABRIC_ENABLE_CUDA)
  include(cuda_probe)
endif()

# ---------------------------------------------------------------------------
# Install and package export
# ---------------------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS cluster_fabric
  EXPORT cluster_fabricTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/cluster_fabric
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")

set(CLUSTER_FABRIC_INSTALL_CMAKEDIR ${CMAKE_INSTALL_LIBDIR}/cmake/cluster_fabric)

install(EXPORT cluster_fabricTargets
  NAMESPACE cluster_fabric::
  FILE cluster_fabricTargets.cmake
  DESTINATION ${CLUSTER_FABRIC_INSTALL_CMAKEDIR})

configure_package_config_file(
  cmake/cluster_fabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/cluster_fabricConfig.cmake
  INSTALL_DESTINATION ${CLUSTER_FABRIC_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/cluster_fabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/cluster_fabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/cluster_fabricConfigVersion.cmake
  DESTINATION ${CLUSTER_FABRIC_INSTALL_CMAKEDIR})

# ---------------------------------------------------------------------------
# Executables, examples, benchmarks, tests
# ---------------------------------------------------------------------------

add_subdirectory(apps)

if(CLUSTER_FABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(CLUSTER_FABRIC_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(CLUSTER_FABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "Cluster Fabric ${PROJECT_VERSION} (${CMAKE_BUILD_TYPE})")
message(STATUS "  C++ standard      : 20")
message(STATUS "  Shared library    : ${BUILD_SHARED_LIBS}")
message(STATUS "  CUDA probe        : ${CLUSTER_FABRIC_CUDA_ENABLED}")
message(STATUS "  Tests             : ${CLUSTER_FABRIC_BUILD_TESTS}")
message(STATUS "  Warnings as errors: ${CLUSTER_FABRIC_WARNINGS_AS_ERRORS}")
