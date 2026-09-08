// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// cluster_fabric_coordinator: runs one cluster coordinator.
//
// This is a real long-running process. It is not a test harness and it never
// terminates itself on a timer: it runs until it is asked to stop.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

void install_stop_handler() {
#if defined(_WIN32)
  SetConsoleCtrlHandler(console_handler, TRUE);
#endif
}

struct Options {
  std::string cluster = "cluster-fabric-lab";
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;
  std::string state_path;
  bool persist = true;
  std::size_t minimum_racks = 1;
  bool allow_partial = true;
  bool allow_degraded = true;
  bool quiet = false;
};

void usage() {
  std::cout << "cluster_fabric_coordinator [options]\n"
               "  --cluster <id>        cluster identity (default cluster-fabric-lab)\n"
               "  --bind <address>      listener address (default 127.0.0.1)\n"
               "  --port <n>            listener port, 0 selects an ephemeral port (default 0)\n"
               "  --state <path>        durable state container path\n"
               "  --no-persist          do not write durable state\n"
               "  --min-racks <n>       readiness contract minimum current racks (default 1)\n"
               "  --no-partial          readiness contract forbids PARTIAL\n"
               "  --no-degraded         readiness contract forbids DEGRADED\n"
               "  --quiet               print only the bound port and terminal lifecycle\n"
               "  --help                print this text\n";
}

bool parse_size(const std::string& text, std::size_t& out) {
  if (text.empty()) {
    return false;
  }
  std::size_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::size_t>(c - '0');
    if (value > 1'000'000) {
      return false;
    }
  }
  out = value;
  return true;
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
    if (arg == "--help" || arg == "-h") {
      usage();
      return false;
    }
    if (arg == "--cluster") {
      if (!next(options.cluster)) return false;
    } else if (arg == "--bind") {
      if (!next(options.bind_address)) return false;
    } else if (arg == "--state") {
      if (!next(options.state_path)) return false;
    } else if (arg == "--port") {
      std::string text;
      if (!next(text)) return false;
      std::size_t value = 0;
      if (!parse_size(text, value) || value > 65535) return false;
      options.port = static_cast<std::uint16_t>(value);
    } else if (arg == "--min-racks") {
      std::string text;
      if (!next(text) || !parse_size(text, options.minimum_racks)) return false;
    } else if (arg == "--no-partial") {
      options.allow_partial = false;
    } else if (arg == "--no-degraded") {
      options.allow_degraded = false;
    } else if (arg == "--no-persist") {
      options.persist = false;
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else {
      std::cerr << "unknown option: " << arg << "\n";
      usage();
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace cluster_fabric;

  Options options;
  if (!parse_options(argc, argv, options)) {
    return 2;
  }
  const std::optional<ClusterId> cluster = ClusterId::parse(options.cluster);
  if (!cluster.has_value()) {
    std::cerr << "invalid cluster identity: " << options.cluster << "\n";
    return 2;
  }
  if (options.minimum_racks == 0) {
    std::cerr << "--min-racks must be at least 1\n";
    return 2;
  }

  std::unique_ptr<FilePersistenceStore> store;
  if (options.persist && !options.state_path.empty()) {
    store = std::make_unique<FilePersistenceStore>(options.state_path);
  }

  CoordinatorConfig config;
  config.cluster = *cluster;
  config.bind_address = options.bind_address;
  config.port = options.port;
  config.persist_on_commit = options.persist && store != nullptr;
  config.readiness_contract = ReadinessContract::permissive();
  config.readiness_contract.minimum_current_racks = options.minimum_racks;
  config.readiness_contract.allow_partial = options.allow_partial;
  config.readiness_contract.allow_degraded = options.allow_degraded;

  ClusterCoordinator coordinator(config, store.get());
  const CoordinatorStartOutcome started = coordinator.start();
  if (!started.ok) {
    std::cerr << "coordinator failed to start: " << to_string(started.status) << " "
              << started.error.message << "\n";
    return 3;
  }

  const RecoveryReport& recovery = coordinator.recovery();
  if (!options.quiet) {
    std::cout << "cluster_fabric_coordinator " << version_banner() << "\n";
    std::cout << "cluster          : " << cluster->value() << "\n";
    std::cout << "listening        : " << options.bind_address << ":" << coordinator.port()
              << "\n";
    std::cout << "durable state    : "
              << (config.persist_on_commit ? options.state_path : std::string("disabled")) << "\n";
    std::cout << "recovered        : " << (recovery.loaded ? "yes" : "no");
    if (recovery.loaded) {
      std::cout << " (racks " << recovery.racks_recovered << " domains "
                << recovery.domains_recovered << " links " << recovery.links_recovered
                << " fenced " << recovery.fenced_recovered << ")";
    }
    std::cout << "\n";
    std::cout << "coordinator epoch: " << recovery.current_coordinator_epoch.str() << "\n";
    for (const std::string& note : recovery.notes) {
      std::cout << "note             : " << note << "\n";
    }
    std::cout.flush();
  } else {
    std::cout << coordinator.port() << "\n";
    std::cout.flush();
  }

  if (!coordinator.state_copy().epoch.known()) {
    MutationRequest request;
    request.kind = MutationKind::DeclareCluster;
    request.cluster = *cluster;
    request.authority.cluster_epoch = ClusterEpoch::from_raw(1);
    request.authority.coordinator_epoch = recovery.current_coordinator_epoch;
    request.declared_lifecycle = ClusterLifecycle::Forming;
    request.readiness_contract = config.readiness_contract;
    request.reason = "coordinator_process_start";
    request.evidence = EvidenceStamp::make(EvidenceProvenance::Reported, default_clock().now(), 0);
    const MutationResult declared = coordinator.submit(request);
    if (!declared.accepted() && !options.quiet) {
      std::cerr << "cluster declaration refused: " << to_string(declared.reason) << " "
                << declared.error.message << "\n";
      return 4;
    }
    if (!options.quiet) {
      std::cout << "declared         : cluster epoch " << declared.cluster_epoch.str() << "\n";
      std::cout.flush();
    }
  }

  install_stop_handler();
  while (!g_stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  const CoordinatorStats stats = coordinator.stats();
  coordinator.stop();
  if (!options.quiet) {
    std::cout << "stopped          : commits " << stats.commits << " accepted "
              << stats.mutations_accepted << " rejected " << stats.mutations_rejected
              << " sessions " << stats.sessions_opened << "\n";
  }
  return 0;
}
