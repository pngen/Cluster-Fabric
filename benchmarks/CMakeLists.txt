# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_executable(bench_cluster bench_cluster.cpp)
target_link_libraries(bench_cluster PRIVATE cluster_fabric::cluster_fabric)
cluster_fabric_strict_options(bench_cluster)

add_executable(bench_queries bench_queries.cpp)
target_link_libraries(bench_queries PRIVATE cluster_fabric::cluster_fabric)
cluster_fabric_strict_options(bench_queries)
