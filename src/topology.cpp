// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Textual rendering of topology classification enums.

#include "cluster_fabric/topology.hpp"

namespace cluster_fabric {

std::string_view to_string(TopologyCurrentness value) noexcept {
  switch (value) {
    case TopologyCurrentness::Unknown: return "UNKNOWN";
    case TopologyCurrentness::Current: return "CURRENT";
    case TopologyCurrentness::StaleEpoch: return "STALE_EPOCH";
    case TopologyCurrentness::StaleGeneration: return "STALE_GENERATION";
    case TopologyCurrentness::RevalidationRequired: return "REVALIDATION_REQUIRED";
  }
  return "UNKNOWN";
}

std::string_view to_string(DomainIndependence value) noexcept {
  switch (value) {
    case DomainIndependence::Unknown: return "UNKNOWN";
    case DomainIndependence::Independent: return "INDEPENDENT";
    case DomainIndependence::Shared: return "SHARED";
    case DomainIndependence::RevalidationRequired: return "REVALIDATION_REQUIRED";
  }
  return "UNKNOWN";
}

}  // namespace cluster_fabric
