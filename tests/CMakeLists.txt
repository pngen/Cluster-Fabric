# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

# One executable per test area. No test declares a CTest TIMEOUT: a test that
# only passes because something killed it is not a passing test.
set(CLUSTER_FABRIC_TEST_AREAS
  test_identity_generation
  test_lifecycle_readiness
  test_membership
  test_topology_domains
  test_snapshot_staleness
  test_persistence
  test_protocol
  test_mutations
  test_fencing_restart
  test_rack_agent
  test_synthetic_property
  test_race_deterministic
  test_process_death
  test_cuda_probe
  test_cli)

foreach(area IN LISTS CLUSTER_FABRIC_TEST_AREAS)
  # A missing source file is skipped rather than failing configuration, which
  # keeps independent work on the suite from blocking a build.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${area}.cpp")
    add_executable(${area} ${area}.cpp)
    target_link_libraries(${area} PRIVATE cluster_fabric::cluster_fabric)
    target_include_directories(${area} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    cluster_fabric_strict_options(${area})
    target_compile_definitions(${area} PRIVATE
      CLUSTER_FABRIC_TEST_COORDINATOR_PATH="$<TARGET_FILE:cluster_fabric_coordinator>"
      CLUSTER_FABRIC_TEST_RACK_AGENT_PATH="$<TARGET_FILE:cluster_fabric_rack_agent>"
      CLUSTER_FABRIC_TEST_INSPECT_PATH="$<TARGET_FILE:cluster_fabric_inspect>")
    if(CLUSTER_FABRIC_CUDA_ENABLED)
      target_link_libraries(${area} PRIVATE cluster_fabric::cluster_fabric_cuda)
      target_compile_definitions(${area} PRIVATE CLUSTER_FABRIC_TEST_HAS_CUDA=1)
    endif()
    add_test(NAME ${area} COMMAND ${area})
    set_tests_properties(${area} PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      LABELS "cluster_fabric")
  endif()
endforeach()
