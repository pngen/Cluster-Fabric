# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

set(CLUSTER_FABRIC_EXAMPLES
  01_embedded_cluster
  02_consumer_contract
  03_cuda_rack_evidence)

foreach(example IN LISTS CLUSTER_FABRIC_EXAMPLES)
  add_executable(${example} ${example}.cpp)
  target_link_libraries(${example} PRIVATE cluster_fabric::cluster_fabric)
  cluster_fabric_strict_options(${example})
endforeach()

if(CLUSTER_FABRIC_CUDA_ENABLED)
  target_link_libraries(03_cuda_rack_evidence PRIVATE cluster_fabric::cluster_fabric_cuda)
endif()
