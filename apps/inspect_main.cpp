// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// cluster_fabric_inspect: read-only inspection of cluster state.
//
// Two sources are supported and neither one mutates anything:
//   --file <path>              decode and validate a durable state container
//   --coordinator <host:port>  query a live coordinator with read-only frames
//
// The live path uses only the public wire protocol. It sends HELLO,
// SNAPSHOT_REQUEST and VALIDATE_SNAPSHOT_REQUEST and never sends a mutation.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "cluster_fabric/cluster_fabric.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Minimal read-only protocol client
// ---------------------------------------------------------------------------

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
#endif

bool socket_start() {
#if defined(_WIN32)
  static const bool ready = []() {
    WSADATA data;
    std::memset(&data, 0, sizeof(data));
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
#else
  return true;
#endif
}

void socket_close(Socket handle) {
  if (handle == kNoSocket) {
    return;
  }
#if defined(_WIN32)
  closesocket(handle);
#else
  ::close(handle);
#endif
}

bool send_all(Socket handle, std::string_view bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const int chunk = ::send(handle, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
    if (chunk <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(chunk);
  }
  return true;
}

bool receive_exact(Socket handle, char* out, std::size_t count) {
  std::size_t received = 0;
  while (received < count) {
    const int chunk = ::recv(handle, out + received, static_cast<int>(count - received), 0);
    if (chunk <= 0) {
      return false;
    }
    received += static_cast<std::size_t>(chunk);
  }
  return true;
}

struct Frame {
  bool ok = false;
  cluster_fabric::MessageType type = cluster_fabric::MessageType::Invalid;
  std::string payload;
  cluster_fabric::StructuredError error;
};

bool exchange(Socket handle, cluster_fabric::MessageType request_type, std::uint64_t sequence,
              const std::string& payload, cluster_fabric::MessageType expected, Frame& out) {
  using namespace cluster_fabric;
  const std::string frame = encode_frame(request_type, sequence, payload);
  if (frame.empty() || !send_all(handle, frame)) {
    out.error = StructuredError::make(ErrorCategory::Transport, ErrorStage::Transport,
                                      "send_failed", "the request frame could not be written");
    return false;
  }
  char header[cluster_fabric::kFrameHeaderSize];
  if (!receive_exact(handle, header, sizeof(header))) {
    out.error = StructuredError::make(ErrorCategory::Transport, ErrorStage::Transport,
                                      "read_failed", "the response header could not be read");
    return false;
  }
  const FrameOutcome header_only =
      decode_frame(std::string_view(header, sizeof(header)), kMaxFramePayloadBytes);
  const std::uint32_t length = header_only.header.payload_length;
  if (length > kMaxFramePayloadBytes) {
    out.error = StructuredError::make(ErrorCategory::Protocol, ErrorStage::Decode,
                                      "oversized_frame",
                                      "the response declares an unacceptable payload length");
    return false;
  }
  std::string buffer(header, sizeof(header));
  buffer.resize(cluster_fabric::kFrameHeaderSize + length);
  if (length != 0 && !receive_exact(handle, buffer.data() + cluster_fabric::kFrameHeaderSize, length)) {
    out.error = StructuredError::make(ErrorCategory::Transport, ErrorStage::Transport,
                                      "read_failed", "the response body could not be read");
    return false;
  }
  const FrameOutcome decoded = decode_frame(buffer, kMaxFramePayloadBytes);
  if (!decoded.ok()) {
    out.error = StructuredError::make(ErrorCategory::Protocol, ErrorStage::Decode,
                                      std::string(to_string(decoded.status)),
                                      "the response frame is not valid");
    return false;
  }
  if (decoded.header.type == MessageType::Error) {
    ErrorMessage message;
    (void)decode_error(decoded.payload, message);
    out.error = message.error;
    return false;
  }
  if (decoded.header.type != expected) {
    out.error = StructuredError::make(ErrorCategory::Protocol, ErrorStage::Decode,
                                      "unexpected_message",
                                      "the coordinator answered with an unexpected message type");
    return false;
  }
  out.ok = true;
  out.type = decoded.header.type;
  out.payload = decoded.payload;
  return true;
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

struct Options {
  std::string file;
  std::string host;
  std::uint16_t port = 0;
  bool json = false;
  bool verbose = false;
};

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[7];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
          out += buffer;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

void usage() {
  std::cout << "cluster_fabric_inspect [options]\n"
               "  --file <path>            inspect a durable state container (read-only)\n"
               "  --coordinator <host:port> inspect a live coordinator (read-only)\n"
               "  --json                   emit machine-readable JSON\n"
               "  --verbose                include per-rack and per-link detail\n"
               "  --help                   print this text\n";
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
    if (value > 65535) {
      return false;
    }
  }
  out = value;
  return true;
}

bool parse_options(int argc, char** argv, Options& options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      usage();
      return false;
    }
    if (arg == "--json") {
      options.json = true;
      continue;
    }
    if (arg == "--verbose") {
      options.verbose = true;
      continue;
    }
    if (arg == "--file" || arg == "--coordinator") {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        return false;
      }
      const std::string value = argv[++i];
      if (arg == "--file") {
        options.file = value;
      } else {
        const std::size_t colon = value.rfind(':');
        if (colon == std::string::npos) {
          std::cerr << "invalid endpoint: " << value << "\n";
          return false;
        }
        std::uint64_t port = 0;
        if (!parse_u64(value.substr(colon + 1), port) || port == 0) {
          std::cerr << "invalid endpoint: " << value << "\n";
          return false;
        }
        options.host = value.substr(0, colon);
        options.port = static_cast<std::uint16_t>(port);
      }
      continue;
    }
    std::cerr << "unknown option: " << arg << "\n";
    return false;
  }
  if (options.file.empty() == options.host.empty()) {
    std::cerr << "exactly one of --file or --coordinator is required\n";
    return false;
  }
  return true;
}

int inspect_file(const Options& options) {
  using namespace cluster_fabric;

  cluster_fabric::FilePersistenceStore store(options.file);
  const PersistenceStore::LoadOutcome loaded = store.load();
  const bool ok = loaded.ok();
  std::string detail;
  PersistenceStatus validation = PersistenceStatus::Ok;
  if (ok) {
    validation = validate_persisted_state(*loaded.state, &detail);
  }

  if (options.json) {
    std::cout << "{\n";
    std::cout << "  \"tool\": \"cluster_fabric_inspect\",\n";
    std::cout << "  \"version\": \"" << json_escape(version_banner()) << "\",\n";
    std::cout << "  \"source\": \"file\",\n";
    std::cout << "  \"path\": \"" << json_escape(options.file) << "\",\n";
    std::cout << "  \"container_status\": \"" << to_string(loaded.status) << "\",\n";
    std::cout << "  \"container_ok\": " << (ok ? "true" : "false") << ",\n";
    if (ok) {
      const PersistedState& state = *loaded.state;
      std::cout << "  \"validation_status\": \"" << to_string(validation) << "\",\n";
      std::cout << "  \"cluster\": \"" << json_escape(state.id.value()) << "\",\n";
      std::cout << "  \"cluster_epoch\": " << state.epoch.value() << ",\n";
      std::cout << "  \"last_coordinator_epoch\": " << state.last_coordinator_epoch.value()
                << ",\n";
      std::cout << "  \"generation\": " << state.generation.value() << ",\n";
      std::cout << "  \"membership_generation\": " << state.membership_generation.value()
                << ",\n";
      std::cout << "  \"topology_epoch\": " << state.topology_epoch.value() << ",\n";
      std::cout << "  \"topology_generation\": " << state.topology_generation.value() << ",\n";
      std::cout << "  \"connectivity_generation\": " << state.connectivity_generation.value()
                << ",\n";
      std::cout << "  \"health_generation\": " << state.health_generation.value() << ",\n";
      std::cout << "  \"lifecycle\": \"" << to_string(state.lifecycle) << "\",\n";
      std::cout << "  \"racks\": " << state.racks.size() << ",\n";
      std::cout << "  \"links\": " << state.links.size() << ",\n";
      std::cout << "  \"constraints\": " << state.constraints.size() << ",\n";
      std::cout << "  \"retired_racks\": " << state.retired_racks.size() << ",\n";
      std::cout << "  \"withdrawn_racks\": " << state.withdrawn_racks.size() << ",\n";
      std::cout << "  \"fenced_authorities\": " << state.fenced_authorities.size() << ",\n";
      std::cout << "  \"placement_domains\": " << state.placement_domains.size() << ",\n";
      std::cout << "  \"capacity_domains\": " << state.capacity_domains.size() << ",\n";
      std::cout << "  \"failure_domains\": " << state.failure_domains.size() << ",\n";
      std::cout << "  \"network_domains\": " << state.network_domains.size() << ",\n";
      std::cout << "  \"storage_domains\": " << state.storage_domains.size() << ",\n";
      std::cout << "  \"power_domains\": " << state.power_domains.size() << ",\n";
      std::cout << "  \"cooling_domains\": " << state.cooling_domains.size() << ",\n";
      std::cout << "  \"link_domains\": " << state.link_domains.size() << ",\n";
      std::cout << "  \"rack_records\": [";
      for (std::size_t i = 0; i < state.racks.size(); ++i) {
        const PersistedState::DurableRack& rack = state.racks[i];
        std::cout << (i == 0 ? "\n" : ",\n");
        std::cout << "    {\"rack\": \"" << json_escape(rack.rack.value())
                  << "\", \"generation\": " << rack.generation.value()
                  << ", \"membership\": \"" << to_string(rack.membership)
                  << "\", \"publisher\": \"" << json_escape(rack.publisher.value())
                  << "\", \"boot\": \"" << json_escape(rack.last_boot.value()) << "\"}";
      }
      std::cout << (state.racks.empty() ? "" : "\n  ") << "]\n";
    } else {
      std::cout << "  \"error\": \"" << json_escape(loaded.error.message) << "\"\n";
    }
    std::cout << "}\n";
    return ok ? 0 : 3;
  }

  std::cout << "cluster_fabric_inspect " << version_banner() << "\n";
  std::cout << "source            : file " << options.file << "\n";
  std::cout << "container         : " << to_string(loaded.status) << "\n";
  if (!ok) {
    std::cout << "detail            : " << loaded.error.message << "\n";
    return 3;
  }
  const PersistedState& state = *loaded.state;
  std::cout << "structural check  : " << to_string(validation);
  if (!detail.empty()) {
    std::cout << " (" << detail << ")";
  }
  std::cout << "\n";
  std::cout << "cluster           : " << state.id.value() << "\n";
  std::cout << "cluster epoch     : " << state.epoch.str() << "\n";
  std::cout << "coordinator epoch : " << state.last_coordinator_epoch.str()
            << " (recorded; a live coordinator advances it on restart)\n";
  std::cout << "generation        : " << state.generation.str() << "\n";
  std::cout << "membership        : " << state.membership_generation.str() << "\n";
  std::cout << "topology          : epoch " << state.topology_epoch.str() << " generation "
            << state.topology_generation.str() << "\n";
  std::cout << "connectivity      : " << state.connectivity_generation.str() << "\n";
  std::cout << "health            : " << state.health_generation.str() << "\n";
  std::cout << "lifecycle         : " << to_string(state.lifecycle) << "\n";
  std::cout << "racks             : " << state.racks.size() << "\n";
  std::cout << "links             : " << state.links.size() << "\n";
  std::cout << "constraints       : " << state.constraints.size() << "\n";
  std::cout << "domains           : placement " << state.placement_domains.size() << " capacity "
            << state.capacity_domains.size() << " failure " << state.failure_domains.size()
            << " network " << state.network_domains.size() << " storage "
            << state.storage_domains.size() << " power " << state.power_domains.size()
            << " cooling " << state.cooling_domains.size() << " link "
            << state.link_domains.size() << "\n";
  std::cout << "retired racks     : " << state.retired_racks.size() << "\n";
  std::cout << "withdrawn racks   : " << state.withdrawn_racks.size() << "\n";
  std::cout << "fenced authorities: " << state.fenced_authorities.size() << "\n";
  if (options.verbose) {
    for (const PersistedState::DurableRack& rack : state.racks) {
      std::cout << "  rack " << rack.rack.value() << " generation " << rack.generation.str()
                << " membership " << to_string(rack.membership) << " publisher "
                << rack.publisher.value() << " boot " << rack.last_boot.value() << "\n";
    }
  }
  return 0;
}

int inspect_coordinator(const Options& options) {
  using namespace cluster_fabric;

  if (!socket_start()) {
    std::cerr << "socket library initialization failed\n";
    return 4;
  }
  const Socket handle = ::socket(AF_INET, SOCK_STREAM, 0);
  if (handle == kNoSocket) {
    std::cerr << "socket creation failed\n";
    return 4;
  }
  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(options.port);
  if (::inet_pton(AF_INET, options.host.c_str(), &address.sin_addr) != 1 ||
      ::connect(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "connect to " << options.host << ":" << options.port << " failed\n";
    socket_close(handle);
    return 4;
  }

  HelloMessage hello;
  hello.cluster = ClusterId::parse("cluster-fabric-inspect").value();
  hello.boot = make_rack_agent_boot_id("inspect");
  hello.role = "inspector";
  hello.build = version_banner();

  Frame response;
  if (!exchange(handle, MessageType::Hello, 1, encode_hello(hello), MessageType::HelloAck,
                response)) {
    std::cerr << "handshake failed: " << response.error.message << "\n";
    socket_close(handle);
    return 4;
  }
  HelloAckMessage ack;
  if (!decode_hello_ack(response.payload, ack).ok() || !ack.accepted) {
    std::cerr << "handshake rejected: " << to_string(ack.reason) << " " << ack.detail << "\n";
    socket_close(handle);
    return 4;
  }

  SnapshotRequestMessage request;
  request.cluster = ack.cluster;
  if (!exchange(handle, MessageType::SnapshotRequest, 2, encode_snapshot_request(request),
                MessageType::SnapshotResponse, response)) {
    std::cerr << "snapshot request failed: " << response.error.message << "\n";
    socket_close(handle);
    return 4;
  }
  SnapshotResponseMessage snapshot;
  if (!decode_snapshot_response(response.payload, snapshot).ok() || !snapshot.accepted) {
    std::cerr << "snapshot refused: " << to_string(snapshot.reason) << " " << snapshot.detail
              << "\n";
    socket_close(handle);
    return 4;
  }

  ValidateSnapshotRequestMessage validate_request;
  validate_request.view = snapshot.view;
  ValidateSnapshotResponseMessage validation;
  if (exchange(handle, MessageType::ValidateSnapshotRequest, 3,
               encode_validate_request(validate_request), MessageType::ValidateSnapshotResponse,
               response)) {
    (void)decode_validate_response(response.payload, validation);
  }
  socket_close(handle);

  const SnapshotView& view = snapshot.view;
  if (options.json) {
    std::cout << "{\n";
    std::cout << "  \"tool\": \"cluster_fabric_inspect\",\n";
    std::cout << "  \"version\": \"" << json_escape(version_banner()) << "\",\n";
    std::cout << "  \"source\": \"coordinator\",\n";
    std::cout << "  \"endpoint\": \"" << json_escape(options.host) << ":" << options.port
              << "\",\n";
    std::cout << "  \"cluster\": \"" << json_escape(view.cluster.value()) << "\",\n";
    std::cout << "  \"coordinator_epoch\": " << view.coordinator_epoch.value() << ",\n";
    std::cout << "  \"cluster_epoch\": " << view.cluster_epoch.value() << ",\n";
    std::cout << "  \"generation\": " << view.cluster_generation.value() << ",\n";
    std::cout << "  \"membership_generation\": " << view.membership_generation.value() << ",\n";
    std::cout << "  \"topology_epoch\": " << view.topology_epoch.value() << ",\n";
    std::cout << "  \"topology_generation\": " << view.topology_generation.value() << ",\n";
    std::cout << "  \"lifecycle\": \"" << to_string(view.lifecycle) << "\",\n";
    std::cout << "  \"rack_count\": " << view.rack_count << ",\n";
    std::cout << "  \"link_count\": " << view.link_count << ",\n";
    std::cout << "  \"domain_count\": " << view.domain_count << ",\n";
    std::cout << "  \"semantic_digest\": \"" << json_escape(view.semantic_digest) << "\",\n";
    std::cout << "  \"current\": " << (validation.validation.current ? "true" : "false")
              << ",\n";
    std::cout << "  \"consumable\": " << (validation.validation.consumable ? "true" : "false")
              << "\n";
    std::cout << "}\n";
    return 0;
  }

  std::cout << "cluster_fabric_inspect " << version_banner() << "\n";
  std::cout << "source            : coordinator " << options.host << ":" << options.port << "\n";
  std::cout << "cluster           : " << view.cluster.value() << "\n";
  std::cout << "coordinator epoch : " << view.coordinator_epoch.str() << "\n";
  std::cout << "cluster epoch     : " << view.cluster_epoch.str() << "\n";
  std::cout << "generation        : " << view.cluster_generation.str() << "\n";
  std::cout << "membership        : " << view.membership_generation.str() << "\n";
  std::cout << "topology          : epoch " << view.topology_epoch.str() << " generation "
            << view.topology_generation.str() << "\n";
  std::cout << "connectivity      : " << view.connectivity_generation.str() << "\n";
  std::cout << "health            : " << view.health_generation.str() << "\n";
  std::cout << "lifecycle         : " << to_string(view.lifecycle) << "\n";
  std::cout << "racks             : " << view.rack_count << "\n";
  std::cout << "links             : " << view.link_count << "\n";
  std::cout << "domains           : " << view.domain_count << "\n";
  std::cout << "semantic digest   : " << view.semantic_digest << "\n";
  std::cout << "snapshot current  : " << (validation.validation.current ? "yes" : "no") << "\n";
  std::cout << "snapshot consumable: " << (validation.validation.consumable ? "yes" : "no")
            << "\n";
  for (std::size_t i = 0; i < validation.validation.reasons.size(); ++i) {
    std::cout << "stale reason      : " << to_string(validation.validation.reasons[i]);
    if (i < validation.validation.subjects.size()) {
      std::cout << " " << validation.validation.subjects[i];
    }
    std::cout << "\n";
  }
  if (options.verbose) {
    for (const RackGenerationBinding& binding : view.rack_bindings) {
      std::cout << "  rack " << binding.rack.value() << " generation "
                << binding.generation.str() << " membership " << to_string(binding.membership)
                << " boot " << binding.boot.value() << "\n";
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    usage();
    return 2;
  }
  return options.file.empty() ? inspect_coordinator(options) : inspect_file(options);
}
