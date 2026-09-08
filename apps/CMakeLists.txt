# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_executable(cluster_fabric_coordinator coordinator_main.cpp)
target_link_libraries(cluster_fabric_coordinator PRIVATE cluster_fabric::cluster_fabric)
cluster_fabric_strict_options(cluster_fabric_coordinator)

add_executable(cluster_fabric_rack_agent rack_agent_main.cpp)
target_link_libraries(cluster_fabric_rack_agent PRIVATE cluster_fabric::cluster_fabric)
cluster_fabric_strict_options(cluster_fabric_rack_agent)

add_executable(cluster_fabric_inspect inspect_main.cpp)
target_link_libraries(cluster_fabric_inspect PRIVATE cluster_fabric::cluster_fabric)
cluster_fabric_strict_options(cluster_fabric_inspect)

# The CUDA probe is a separate optional component. When it is built, the rack
# agent exposes the real measured GPU path; otherwise run_cuda_probe reports
# cuda_not_compiled and the rack reference stays UNKNOWN.
if(CLUSTER_FABRIC_CUDA_ENABLED)
  target_link_libraries(cluster_fabric_rack_agent PRIVATE cluster_fabric::cluster_fabric_cuda)
  target_compile_definitions(cluster_fabric_rack_agent PRIVATE CLUSTER_FABRIC_APP_HAS_CUDA=1)
endif()

include(GNUInstallDirs)
install(TARGETS cluster_fabric_coordinator cluster_fabric_rack_agent cluster_fabric_inspect
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
