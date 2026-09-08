// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// cluster_fabric_rack_agent: one rack-side publisher process.
//
// A rack agent is an independent operating-system process. Killing it is the
// real process-death proof: the coordinator fences its incarnation and the
// evidence it owned becomes revalidation-required.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "cluster_fabric/cluster_fabric.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_stop_requested{false};

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
      type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
    g_stop_requested.store(true);
    return TRUE;
  }
  return FALSE;
}
#endif

struct Options {
  std::string cluster = "cluster-fabric-lab";
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  std::string rack = "rack-000";
  std::string publisher = "rack-publisher";
  std::uint64_t generation = 1;
  std::int64_t ttl_millis = 30'000;
  std::int64_t heartbeat_millis = 1'000;
  std::size_t devices = 8;
  std::uint64_t device_memory_bytes = 32ull * 1024 * 1024 * 1024;
  bool gpu = false;
  bool publish_once = false;
  std::size_t supersede_after = 0;
};

void usage() {
  std::cout << "cluster_fabric_rack_agent [options]\n"
               "  --cluster <id>        cluster identity (default cluster-fabric-lab)\n"
               "  --coordinator <h:p>   coordinator endpoint, for example 127.0.0.1:50051\n"
               "  --rack <id>           rack identity (default rack-000)\n"
               "  --publisher <id>      durable publisher authority (default rack-publisher)\n"
               "  --generation <n>      rack generation to publish (default 1)\n"
               "  --ttl <ms>            evidence time-to-live (default 30000)\n"
               "  --heartbeat <ms>      heartbeat interval (default 1000)\n"
               "  --devices <n>         accelerator count in the synthetic composition (default 8)\n"
               "  --device-memory <mb>  device memory per accelerator (default 32768)\n"
               "  --gpu                 measure the local CUDA device and publish MEASURED evidence\n"
               "  --supersede-after <n> advance the rack generation after n publications\n"
               "  --publish-once        publish once and exit\n"
               "  --help                print this text\n";
}

bool parse_u64(const std::string& text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
    if (value > 1'000'000'000'000ull) {
      return false;
    }
  }
  out = value;
  return true;
}

bool parse_endpoint(const std::string& text, std::string& host, std::uint16_t& port) {
  const std::size_t colon = text.rfind(':');
  if (colon == std::string::npos) {
    return false;
  }
  host = text.substr(0, colon);
  std::uint64_t value = 0;
  if (!parse_u64(text.substr(colon + 1), value) || value == 0 || value > 65535) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return !host.empty();
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto next = [&argc, &argv, &i](std::string& target) {
      if (i + 1 >= argc) {
        return false;
      }
      target = argv[++i];
      return true;
    };
    std::string text;
    if (arg == "--help" || arg == "-h") {
      usage();
      return false;
    } else if (arg == "--cluster") {
      if (!next(options.cluster)) return false;
    } else if (arg == "--coordinator") {
      if (!next(text) || !parse_endpoint(text, options.host, options.port)) {
        std::cerr << "invalid coordinator endpoint: " << text << "\n";
        return false;
      }
    } else if (arg == "--rack") {
      if (!next(options.rack)) return false;
    } else if (arg == "--publisher") {
      if (!next(options.publisher)) return false;
    } else if (arg == "--generation") {
      if (!next(text) || !parse_u64(text, options.generation)) return false;
    } else if (arg == "--ttl") {
      std::uint64_t value = 0;
      if (!next(text) || !parse_u64(text, value)) return false;
      options.ttl_millis = static_cast<std::int64_t>(value);
    } else if (arg == "--heartbeat") {
      std::uint64_t value = 0;
      if (!next(text) || !parse_u64(text, value) || value == 0) return false;
      options.heartbeat_millis = static_cast<std::int64_t>(value);
    } else if (arg == "--devices") {
      std::uint64_t value = 0;
      if (!next(text) || !parse_u64(text, value)) return false;
      options.devices = static_cast<std::size_t>(value);
    } else if (arg == "--device-memory") {
      std::uint64_t megabytes = 0;
      if (!next(text) || !parse_u64(text, megabytes)) return false;
      options.device_memory_bytes = megabytes * 1024ull * 1024ull;
    } else if (arg == "--supersede-after") {
      std::uint64_t value = 0;
      if (!next(text) || !parse_u64(text, value)) return false;
      options.supersede_after = static_cast<std::size_t>(value);
    } else if (arg == "--gpu") {
      options.gpu = true;
    } else if (arg == "--publish-once") {
      options.publish_once = true;
    } else {
      std::cerr << "unknown option: " << arg << "\n";
      usage();
      return false;
    }
  }
  return options.port != 0;
}

void install_stop_handler() {
#if defined(_WIN32)
  SetConsoleCtrlHandler(console_handler, TRUE);
#endif
}

}  // namespace

int main(int argc, char** argv) {
  using namespace cluster_fabric;

  Options options;
  if (!parse_options(argc, argv, options)) {
    usage();
    return 2;
  }
  const std::optional<ClusterId> cluster = ClusterId::parse(options.cluster);
  const std::optional<RackId> rack = RackId::parse(options.rack);
  const std::optional<RackPublisherId> publisher = RackPublisherId::parse(options.publisher);
  if (!cluster.has_value() || !rack.has_value() || !publisher.has_value()) {
    std::cerr << "invalid cluster, rack or publisher identity\n";
    return 2;
  }

  RackAgentConfig config;
  config.cluster = *cluster;
  config.rack = *rack;
  config.generation = RackGeneration::from_raw(options.generation);
  config.publisher = *publisher;
  config.coordinator_host = options.host;
  config.coordinator_port = options.port;
  config.publication_ttl_millis = options.ttl_millis;
  config.origin_label = "cluster-fabric:rack-agent";

  bool measured = false;
  if (options.gpu) {
    const CudaProbeResult probe = run_cuda_probe(1u << 20, 0);
    if (probe.success) {
      measured = true;
      config.rack_lifecycle = RackLifecycleState::Ready;
      config.health = HealthState::Healthy;
      config.composition =
          make_cuda_rack_reference(probe, *rack, config.generation, *publisher).composition;
      std::cout << "gpu              : " << probe.device.name << " (" << probe.device.uuid
                << ")\n";
      std::cout << "gpu memory       : " << probe.device.total_memory_bytes << " bytes\n";
      std::cout << "probe            : h2d " << probe.h2d_millis << " ms kernel "
                << probe.kernel_millis << " ms d2h " << probe.d2h_millis << " ms max_abs_error "
                << probe.max_abs_error << " digest " << probe.output_digest << "\n";
    } else {
      std::cout << "gpu              : unavailable (" << probe.reason << ": " << probe.detail
                << "); publishing UNKNOWN device facts rather than inventing them\n";
    }
  }
  if (!measured) {
    AcceleratorClassSummary summary;
    summary.vendor = AcceleratorVendor::Other;
    summary.family = "synthetic-accelerator";
    summary.device_count = static_cast<std::uint32_t>(options.devices);
    summary.device_memory_bytes = options.device_memory_bytes;
    config.composition.accelerators.push_back(std::move(summary));
    config.composition.cpu_sockets = 2u;
    config.composition.cpu_cores = 128u;
    config.composition.host_memory_bytes = 1024ull * 1024 * 1024 * 1024;
    config.composition.nic_count = 2u;
    config.composition.composition_label = "synthetic-rack-agent-composition";
    config.composition.provenance = EvidenceProvenance::Synthetic;
  }

  RackEndpoint endpoint;
  endpoint.id = *RackEndpointId::parse(measured ? "gpu0" : "fabric0");
  endpoint.connectivity = measured ? ConnectivityClass::DirectFabric : ConnectivityClass::Unknown;
  endpoint.evidence = EvidenceStamp::make(
      measured ? EvidenceProvenance::Measured : EvidenceProvenance::Synthetic,
      default_clock().now(), options.ttl_millis);
  config.endpoints.push_back(std::move(endpoint));

  RackFailureDomainHint hint;
  hint.klass = FailureDomainClass::Rack;
  hint.id = *FailureDomainId::parse(options.rack);
  hint.evidence = EvidenceStamp::make(
      measured ? EvidenceProvenance::Measured : EvidenceProvenance::Synthetic,
      default_clock().now(), options.ttl_millis);
  config.failure_domain_hints.push_back(std::move(hint));

  RackAgent agent(std::move(config));
  install_stop_handler();

  if (!agent.connect()) {
    const RackAgentStatus& status = agent.status();
    std::cerr << "connect failed: " << to_string(status.state) << " "
              << to_string(status.transport) << " " << status.detail << "\n";
    return 3;
  }
  std::cout << "connected        : coordinator epoch "
            << agent.status().coordinator_epoch.str() << " cluster epoch "
            << agent.status().cluster_epoch.str() << " topology epoch "
            << agent.status().topology_epoch.str() << "\n";

  std::size_t publications = 0;
  MutationResult published = agent.publish();
  if (!published.accepted()) {
    std::cerr << "publish refused: " << to_string(published.reason) << " "
              << published.error.message << "\n";
    agent.stop();
    return 4;
  }
  ++publications;
  std::cout << "published        : generation " << published.cluster_generation.str()
            << " membership " << published.membership_generation.str() << " lifecycle "
            << to_string(published.lifecycle) << "\n";
  if (options.supersede_after != 0 && publications >= options.supersede_after) {
    const MutationResult superseded =
        agent.supersede(RackGeneration::from_raw(options.generation + 1));
    std::cout << "superseded       : " << to_string(superseded.outcome) << " "
              << to_string(superseded.reason) << "\n";
  }

  if (options.publish_once) {
    agent.stop();
    return 0;
  }

  std::size_t heartbeats = 0;
  while (!g_stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(options.heartbeat_millis));
    if (g_stop_requested.load()) {
      break;
    }
    const MutationResult beat = agent.heartbeat();
    if (!beat.accepted()) {
      std::cerr << "heartbeat refused: " << to_string(beat.reason) << " " << beat.error.message
                << "\n";
      break;
    }
    ++heartbeats;
  }

  agent.stop();
  std::cout << "stopped          : publications " << publications << " heartbeats " << heartbeats
            << " state " << to_string(agent.status().state) << "\n";
  return 0;
}
