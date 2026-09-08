// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Framed TCP transport, session handling and coordinator shutdown.
//
// This build is validated as a single-host multiprocess reference deployment:
// real Winsock TCP over loopback, real independent operating-system processes,
// real process death. It is not a multi-node cluster networking claim.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cluster_fabric/limits.hpp"
#include "cluster_fabric/version.hpp"
#include "detail/coordinator_impl.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cluster_fabric {
namespace detail {

bool socket_library_init() noexcept {
#if defined(_WIN32)
  static const bool initialized = []() {
    WSADATA data;
    std::memset(&data, 0, sizeof(data));
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
#else
  return true;
#endif
}

std::string socket_error_text() noexcept {
#if defined(_WIN32)
  const int code = WSAGetLastError();
  return "winsock error " + std::to_string(code);
#else
  return "socket error " + std::to_string(errno);
#endif
}

void close_socket(socket_handle handle) noexcept {
  if (handle == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  closesocket(static_cast<SOCKET>(handle));
#else
  ::close(handle);
#endif
}

void shutdown_socket(socket_handle handle) noexcept {
  if (handle == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  ::shutdown(static_cast<SOCKET>(handle), SD_BOTH);
#else
  ::shutdown(handle, SHUT_RDWR);
#endif
}

socket_handle open_listener(const std::string& bind_address, std::uint16_t port,
                            std::size_t backlog) noexcept {
  const socket_handle handle = static_cast<socket_handle>(::socket(AF_INET, SOCK_STREAM, 0));
  if (handle == kInvalidSocket) {
    return kInvalidSocket;
  }
  int reuse = 1;
  ::setsockopt(static_cast<socket_handle>(handle), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), static_cast<int>(sizeof(reuse)));

  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (::inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
    close_socket(handle);
    return kInvalidSocket;
  }
  if (::bind(handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    close_socket(handle);
    return kInvalidSocket;
  }
  const std::size_t effective_backlog = (std::max)(backlog, static_cast<std::size_t>(16));
  if (::listen(handle, static_cast<int>(effective_backlog)) != 0) {
    close_socket(handle);
    return kInvalidSocket;
  }
  return handle;
}

std::uint16_t listener_port(socket_handle handle) noexcept {
  if (handle == kInvalidSocket) {
    return 0;
  }
  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  int length = static_cast<int>(sizeof(address));
  if (::getsockname(handle, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
  return ntohs(address.sin_port);
}

}  // namespace detail

namespace {

enum class TransferResult { Ok, Closed, Error, Stopped };

/// Waits until the handle is readable. Returns false on error, true on
/// readability or poll expiry. The poll interval exists only so a session can
/// observe the shutdown flag; it is not an operation timeout.
[[nodiscard]] bool wait_readable(detail::socket_handle handle) noexcept {
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(static_cast<detail::socket_handle>(handle), &read_set);
  timeval interval;
  interval.tv_sec = 0;
  interval.tv_usec = 100'000;
#if defined(_WIN32)
  const int result = ::select(0, &read_set, nullptr, nullptr, &interval);
#else
  const int result = ::select(static_cast<int>(handle) + 1, &read_set, nullptr, nullptr, &interval);
#endif
  return result >= 0;
}

[[nodiscard]] TransferResult receive_exact(detail::socket_handle handle, char* out, std::size_t count,
                                           const std::atomic<bool>& stop) {
  std::size_t received = 0;
  while (received < count) {
    if (stop.load()) {
      return TransferResult::Stopped;
    }
    if (!wait_readable(handle)) {
      return TransferResult::Error;
    }
    const int chunk = ::recv(handle, out + received, static_cast<int>(count - received), 0);
    if (chunk == 0) {
      return TransferResult::Closed;
    }
    if (chunk < 0) {
#if defined(_WIN32)
      const int code = WSAGetLastError();
      if (code == WSAEINTR || code == WSAEWOULDBLOCK) {
        continue;
      }
#else
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
#endif
      return TransferResult::Error;
    }
    received += static_cast<std::size_t>(chunk);
  }
  return TransferResult::Ok;
}

[[nodiscard]] bool send_all(detail::socket_handle handle, std::string_view bytes) noexcept {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
    const int chunk = ::send(handle, bytes.data() + sent,
                             static_cast<int>(bytes.size() - sent), 0);
    if (chunk <= 0) {
#if defined(_WIN32)
      if (chunk < 0 && WSAGetLastError() == WSAEINTR) {
        continue;
      }
#else
      if (chunk < 0 && errno == EINTR) {
        continue;
      }
#endif
      return false;
    }
    sent += static_cast<std::size_t>(chunk);
  }
  return true;
}

[[nodiscard]] std::string frame_for(MessageType type, std::uint64_t sequence,
                                    const std::string& payload, std::uint32_t max_payload) {
  return encode_frame(type, sequence, payload, 0, max_payload);
}

}  // namespace

SnapshotValidation ClusterCoordinator::Impl::validate_view(const SnapshotView& view) {
  return with_state([&view](const ClusterState& state, const ClusterIndexes&) {
    SnapshotValidation validation;
    auto add = [&validation](SnapshotStaleReason reason, std::string subject) {
      validation.reasons.push_back(reason);
      validation.subjects.push_back(std::move(subject));
    };
    if (view.cluster != state.id) {
      add(SnapshotStaleReason::WrongCluster, "snapshot/" + view.cluster.value());
    }
    if (view.cluster_epoch != state.epoch) {
      add(SnapshotStaleReason::ClusterEpochAdvanced,
          "bound=" + view.cluster_epoch.str() + " current=" + state.epoch.str());
    }
    if (view.coordinator_epoch != state.coordinator_epoch) {
      add(SnapshotStaleReason::CoordinatorEpochAdvanced,
          "bound=" + view.coordinator_epoch.str() + " current=" + state.coordinator_epoch.str());
    }
    if (view.cluster_generation != state.generation) {
      add(SnapshotStaleReason::ClusterGenerationAdvanced,
          "bound=" + view.cluster_generation.str() + " current=" + state.generation.str());
    }
    if (view.membership_generation != state.membership_generation) {
      add(SnapshotStaleReason::MembershipChanged,
          "bound=" + view.membership_generation.str() + " current=" +
              state.membership_generation.str());
    }
    if (view.topology_epoch != state.topology_epoch) {
      add(SnapshotStaleReason::TopologyEpochSuperseded,
          "bound=" + view.topology_epoch.str() + " current=" + state.topology_epoch.str());
    } else if (view.topology_generation != state.topology_generation) {
      add(SnapshotStaleReason::TopologyGenerationAdvanced,
          "bound=" + view.topology_generation.str() + " current=" +
              state.topology_generation.str());
    }
    if (view.domain_generations.placement != state.domain_generations.placement) {
      add(SnapshotStaleReason::PlacementDomainSuperseded, "placement_domain");
    }
    if (view.domain_generations.capacity != state.domain_generations.capacity) {
      add(SnapshotStaleReason::CapacityDomainSuperseded, "capacity_domain");
    }
    if (view.domain_generations.failure != state.domain_generations.failure) {
      add(SnapshotStaleReason::FailureDomainSuperseded, "failure_domain");
    }
    if (view.domain_generations.network != state.domain_generations.network) {
      add(SnapshotStaleReason::NetworkDomainSuperseded, "network_domain");
    }
    if (view.domain_generations.storage != state.domain_generations.storage) {
      add(SnapshotStaleReason::StorageDomainSuperseded, "storage_domain");
    }
    if (view.domain_generations.power != state.domain_generations.power) {
      add(SnapshotStaleReason::PowerDomainSuperseded, "power_domain");
    }
    if (view.domain_generations.cooling != state.domain_generations.cooling) {
      add(SnapshotStaleReason::CoolingDomainSuperseded, "cooling_domain");
    }
    if (view.domain_generations.link != state.domain_generations.link) {
      add(SnapshotStaleReason::LinkDomainSuperseded, "link_domain");
    }
    if (view.connectivity_generation != state.connectivity_generation) {
      add(SnapshotStaleReason::ConnectivitySuperseded,
          "bound=" + view.connectivity_generation.str() + " current=" +
              state.connectivity_generation.str());
    }
    if (view.health_generation != state.health_generation) {
      add(SnapshotStaleReason::HealthSuperseded,
          "bound=" + view.health_generation.str() + " current=" +
              state.health_generation.str());
    }
    for (const RackGenerationBinding& binding : view.rack_bindings) {
      const auto it = state.racks.find(binding.rack);
      if (it == state.racks.end()) {
        add(SnapshotStaleReason::RackWithdrawn, "rack/" + binding.rack.value());
        continue;
      }
      if (state.is_rack_retired(binding.rack)) {
        add(SnapshotStaleReason::RackRetired, "rack/" + binding.rack.value());
        continue;
      }
      if (it->second.reference.generation != binding.generation) {
        add(SnapshotStaleReason::RackGenerationSuperseded,
            "rack/" + binding.rack.value() + " bound=" + binding.generation.str() +
                " current=" + it->second.reference.generation.str());
      }
      if (it->second.membership != binding.membership) {
        add(SnapshotStaleReason::RackWithdrawn, "rack/" + binding.rack.value());
      }
      if (state.is_boot_fenced(binding.boot)) {
        add(SnapshotStaleReason::RackBootFenced, "rack/" + binding.rack.value());
      }
      if (it->second.reference.currentness == RackCurrentness::RevalidationRequired ||
          it->second.reference.evidence.requires_revalidation()) {
        add(SnapshotStaleReason::RackRevalidationRequired, "rack/" + binding.rack.value());
      }
    }
    for (const auto& entry : state.racks) {
      const bool present = std::any_of(view.rack_bindings.begin(), view.rack_bindings.end(),
                                       [&entry](const RackGenerationBinding& binding) {
                                         return binding.rack == entry.first;
                                       });
      if (!present) {
        add(SnapshotStaleReason::MembershipChanged, "rack/" + entry.first.value());
      }
    }
    if (!is_consumable_lifecycle(state.lifecycle)) {
      add(SnapshotStaleReason::LifecycleNotConsumable, std::string(to_string(state.lifecycle)));
    }
    validation.current = validation.reasons.empty();
    validation.consumable = validation.current && is_consumable_lifecycle(state.lifecycle);
    validation.explanation = Explanation::make(validation.current ? "snapshot_current"
                                                                  : "snapshot_stale",
                                               "snapshot", validation.describe());
    for (std::size_t i = 0; i < validation.reasons.size(); ++i) {
      validation.explanation.add(std::string(to_string(validation.reasons[i])),
                                 i < validation.subjects.size() ? validation.subjects[i]
                                                                : std::string(),
                                 "supplied snapshot view no longer matches current authority");
    }
    validation.explanation.sort_factors();
    validation.explanation.bound_factors();
    return validation;
  });
}

bool ClusterCoordinator::Impl::send_frame(detail::Session& session, MessageType type,
                                          std::uint64_t sequence, const std::string& payload) {
  const std::string frame = frame_for(type, sequence, payload, config.max_frame_payload);
  if (frame.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(session.write_mutex);
  if (session.handle == detail::kInvalidSocket) {
    return false;
  }
  if (!send_all(session.handle, frame)) {
    return false;
  }
  ++frames_out;
  bytes_out += frame.size();
  return true;
}

void ClusterCoordinator::Impl::detach_session(std::uint64_t id) {
  std::shared_ptr<detail::Session> session;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    const auto it = sessions.find(id);
    if (it != sessions.end()) {
      session = it->second;
      sessions.erase(it);
    }
  }
  if (session != nullptr && session->boot.has_value()) {
    const RackAgentBootId boot = *session->boot;
    // Only an incarnation that currently owns live evidence is fenced. A
    // read-only client (a HELLO plus snapshot query) must never grow the
    // fenced-authority set.
    const bool owns_evidence = with_state([&boot](const ClusterState& state, const ClusterIndexes&) {
      for (const auto& entry : state.racks) {
        if (entry.second.reference.boot == boot) {
          return true;
        }
      }
      for (const auto& entry : state.links) {
        if (entry.second.boot.has_value() && *entry.second.boot == boot) {
          return true;
        }
      }
      return false;
    });
    if (!owns_evidence) {
      ++sessions_closed;
      return;
    }
    const bool fenced = config.fence_on_disconnect;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      if (!stopping) {
        detail::QueueItem item;
        item.kind = detail::QueueItem::Kind::Fence;
        item.fence_boot = boot;
        item.fence_reason = fenced ? "session_closed" : "session_closed_evidence_stale";
        queue.push_back(std::move(item));
      }
    }
    queue_condition.notify_one();
  }
  ++sessions_closed;
}

void ClusterCoordinator::Impl::run_accept() {
  while (accepting.load()) {
    sockaddr_in peer;
    std::memset(&peer, 0, sizeof(peer));
    int length = static_cast<int>(sizeof(peer));
    const detail::socket_handle handle = static_cast<detail::socket_handle>(
        ::accept(listener, reinterpret_cast<sockaddr*>(&peer), &length));
    if (handle == detail::kInvalidSocket) {
      if (!accepting.load()) {
        return;
      }
      continue;
    }
    bool admitted = false;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      admitted = sessions.size() < config.max_sessions;
    }
    if (!admitted) {
      detail::shutdown_socket(handle);
      detail::close_socket(handle);
      continue;
    }
    auto session = std::make_shared<detail::Session>();
    session->id = next_session_id.fetch_add(1);
    session->handle = handle;
    {
      std::lock_guard<std::mutex> lock(sessions_mutex);
      sessions[session->id] = session;
    }
    ++sessions_opened;
    std::thread worker([this, session]() { run_session(session); });
    worker.detach();
  }
}

void ClusterCoordinator::Impl::close_all_sessions() {
  std::vector<std::shared_ptr<detail::Session>> snapshot;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex);
    for (const auto& entry : sessions) {
      snapshot.push_back(entry.second);
    }
  }
  for (const std::shared_ptr<detail::Session>& session : snapshot) {
    session->stop.store(true);
    detail::shutdown_socket(session->handle);
  }
}

void ClusterCoordinator::Impl::run_session(const std::shared_ptr<detail::Session>& session) {
  std::uint64_t sequence = 0;
  std::uint32_t max_payload = config.max_frame_payload;
  if (max_payload == 0 || max_payload > kAbsoluteMaxFramePayloadBytes) {
    max_payload = kMaxFramePayloadBytes;
  }
  bool handshake_done = false;

  for (;;) {
    if (session->stop.load()) {
      break;
    }
    char header_bytes[kFrameHeaderSize];
    const TransferResult header_result =
        receive_exact(session->handle, header_bytes, sizeof(header_bytes), session->stop);
    if (header_result != TransferResult::Ok) {
      break;
    }
    FrameOutcome frame = decode_frame(std::string_view(header_bytes, sizeof(header_bytes)),
                                      max_payload);
    if (frame.status == ProtocolStatus::TruncatedPayload) {
      // The header declared a payload that has not arrived yet: read it.
      const std::uint32_t payload_length = frame.header.payload_length;
      if (payload_length > max_payload) {
        ErrorMessage error;
        error.error = StructuredError::make(ErrorCategory::Protocol, ErrorStage::Decode,
                                            "oversized_frame",
                                            "declared payload exceeds the configured maximum");
        send_frame(*session, MessageType::Error, ++sequence, encode_error(error));
        break;
      }
      std::string buffer(header_bytes, sizeof(header_bytes));
      buffer.resize(kFrameHeaderSize + payload_length);
      const TransferResult body_result = receive_exact(session->handle,
                                                       buffer.data() + kFrameHeaderSize,
                                                       payload_length, session->stop);
      if (body_result != TransferResult::Ok) {
        break;
      }
      frame = decode_frame(buffer, max_payload);
    }
    if (!frame.ok()) {
      ErrorMessage error;
      error.error = StructuredError::make(ErrorCategory::Protocol, ErrorStage::Decode,
                                          std::string(to_string(frame.status)),
                                          "frame rejected before any state was touched");
      send_frame(*session, MessageType::Error, ++sequence, encode_error(error));
      break;
    }
    ++frames_in;
    bytes_in += frame.consumed;

    const std::string& payload = frame.payload;
    const MessageType type = frame.header.type;

    if (type == MessageType::Hello) {
      HelloMessage hello;
      const DecodeOutcome decoded = decode_hello(payload, hello);
      HelloAckMessage ack;
      ack.cluster = config.cluster;
      if (!decoded.ok()) {
        ack.accepted = false;
        ack.reason = RejectionReason::Malformed;
        ack.detail = "HELLO payload is malformed";
      } else if (hello.protocol_version != kProtocolVersion) {
        ack.accepted = false;
        ack.reason = RejectionReason::Malformed;
        ack.detail = "unsupported protocol version";
      } else if (hello.cluster != config.cluster) {
        ack.accepted = false;
        ack.reason = RejectionReason::WrongCluster;
        ack.detail = "peer targets a different cluster";
      } else {
        with_state([&ack](const ClusterState& state, const ClusterIndexes&) {
          ack.accepted = true;
          ack.coordinator_epoch = state.coordinator_epoch;
          ack.cluster_epoch = state.epoch;
          ack.topology_epoch = state.topology_epoch;
          ack.topology_generation = state.topology_generation;
          ack.lifecycle = state.lifecycle;
          return 0;
        });
        session->boot = hello.boot;
        handshake_done = true;
      }
      send_frame(*session, MessageType::HelloAck, ++sequence, encode_hello_ack(ack));
      if (!ack.accepted) {
        break;
      }
      continue;
    }

    if (!handshake_done) {
      ErrorMessage error;
      error.error = StructuredError::make(ErrorCategory::Protocol, ErrorStage::ValidateAuthority,
                                          "handshake_required",
                                          "HELLO must be the first message on a session");
      send_frame(*session, MessageType::Error, ++sequence, encode_error(error));
      break;
    }

    switch (type) {
      case MessageType::RegisterPublisher: {
        RegisterPublisherMessage message;
        const DecodeOutcome decoded = decode_register_publisher(payload, message);
        RegisterAckMessage ack;
        if (!decoded.ok()) {
          ack.accepted = false;
          ack.reason = RejectionReason::Malformed;
          ack.detail = "REGISTER payload is malformed";
        } else {
          MutationRequest request;
          request.kind = MutationKind::RegisterRackPublisher;
          request.cluster = message.cluster;
          request.authority = message.authority;
          request.authority.boot = session->boot;
          request.authority.publisher = message.publisher;
          request.authority.rack = message.rack;
          request.authority.rack_generation = message.generation;
          request.rack_reference.rack = message.rack;
          request.rack_reference.generation = message.generation;
          request.rack_reference.rack_lifecycle = message.rack_lifecycle;
          request.rack_reference.composition = message.composition;
          request.rack_reference.origin_label = message.origin_label;
          request.rack_reference.publisher = message.publisher;
          request.rack_reference.boot = session->boot.value_or(message.authority.boot.value());
          request.rack_reference.publication = message.authority.publication;
          request.evidence = EvidenceStamp::make(EvidenceProvenance::Reported, now(), 0);
          request.reason = "rack_publisher_registration";
          const MutationResult result = submit(request);
          ack.accepted = result.accepted();
          ack.reason = result.reason;
          ack.detail = result.explanation.summary;
          ack.coordinator_epoch = result.coordinator_epoch;
          ack.cluster_epoch = result.cluster_epoch;
          ack.topology_epoch = result.topology_epoch;
          ack.topology_generation = result.topology_generation;
          ack.membership_generation = result.membership_generation;
          if (ack.accepted) {
            session->racks.push_back(message.rack);
          }
        }
        send_frame(*session, MessageType::RegisterAck, ++sequence, encode_register_ack(ack));
        break;
      }
      case MessageType::PublishRequest: {
        PublishRequestMessage message;
        const DecodeOutcome decoded = decode_publish_request(payload, message);
        PublishResultMessage response;
        if (!decoded.ok()) {
          response.result = MutationResult::rejected(
              RejectionReason::Malformed, ErrorStage::Decode, "request",
              "PUBLISH payload is malformed", "malformed_payload");
        } else {
          MutationRequest request = message.request;
          if (session->boot.has_value()) {
            request.authority.boot = session->boot;
          }
          response.result = submit(request);
        }
        send_frame(*session, MessageType::PublishResult, ++sequence,
                   encode_publish_result(response));
        break;
      }
      case MessageType::Heartbeat: {
        HeartbeatMessage message;
        const DecodeOutcome decoded = decode_heartbeat(payload, message);
        HeartbeatAckMessage ack;
        if (!decoded.ok()) {
          ack.accepted = false;
          ack.reason = RejectionReason::Malformed;
          ack.detail = "HEARTBEAT payload is malformed";
        } else {
          with_state([&ack, &message, this](const ClusterState& state, const ClusterIndexes&) {
            ack.coordinator_epoch = state.coordinator_epoch;
            ack.cluster_epoch = state.epoch;
            ack.topology_epoch = state.topology_epoch;
            ack.lifecycle = state.lifecycle;
            if (message.cluster != config.cluster) {
              ack.accepted = false;
              ack.reason = RejectionReason::WrongCluster;
              ack.detail = "heartbeat targets a different cluster";
            } else if (message.authority.coordinator_epoch != state.coordinator_epoch) {
              ack.accepted = false;
              ack.reason = RejectionReason::StaleCoordinatorEpoch;
              ack.detail = "heartbeat was authored under a superseded coordinator epoch";
            } else if (message.authority.cluster_epoch != state.epoch) {
              ack.accepted = false;
              ack.reason = RejectionReason::StaleClusterEpoch;
              ack.detail = "heartbeat was authored under a superseded cluster epoch";
            } else {
              ack.accepted = true;
            }
            return 0;
          });
        }
        send_frame(*session, MessageType::HeartbeatAck, ++sequence, encode_heartbeat_ack(ack));
        break;
      }
      case MessageType::SnapshotRequest: {
        SnapshotRequestMessage message;
        const DecodeOutcome decoded = decode_snapshot_request(payload, message);
        SnapshotResponseMessage response;
        if (!decoded.ok()) {
          response.accepted = false;
          response.reason = RejectionReason::Malformed;
          response.detail = "SNAPSHOT_REQUEST payload is malformed";
        } else if (message.cluster != config.cluster) {
          response.accepted = false;
          response.reason = RejectionReason::WrongCluster;
          response.detail = "snapshot request targets a different cluster";
        } else {
          response.accepted = true;
          response.view = SnapshotView::from_snapshot(make_snapshot());
        }
        send_frame(*session, MessageType::SnapshotResponse, ++sequence,
                   encode_snapshot_response(response));
        break;
      }
      case MessageType::ValidateSnapshotRequest: {
        ValidateSnapshotRequestMessage message;
        const DecodeOutcome decoded = decode_validate_request(payload, message);
        ValidateSnapshotResponseMessage response;
        if (!decoded.ok()) {
          response.validation.explanation =
              Explanation::make("malformed_payload", "snapshot", "VALIDATE payload is malformed");
        } else {
          response.validation = validate_view(message.view);
        }
        send_frame(*session, MessageType::ValidateSnapshotResponse, ++sequence,
                   encode_validate_response(response));
        break;
      }
      case MessageType::RevalidateRequest: {
        RevalidateRequestMessage message;
        const DecodeOutcome decoded = decode_revalidate_request(payload, message);
        RevalidateResponseMessage response;
        if (!decoded.ok()) {
          response.accepted = false;
          response.reason = RejectionReason::Malformed;
          response.detail = "REVALIDATE payload is malformed";
        } else {
          MutationRequest request;
          request.kind = MutationKind::RevalidateRecoveredState;
          request.cluster = message.cluster;
          request.authority = message.authority;
          if (session->boot.has_value()) {
            request.authority.boot = session->boot;
          }
          request.racks = message.racks;
          request.evidence = EvidenceStamp::make(EvidenceProvenance::Reported, now(), 0);
          const MutationResult result = submit(request);
          response.accepted = result.accepted();
          response.reason = result.reason;
          response.detail = result.explanation.summary;
          response.lifecycle = result.lifecycle;
          response.readiness = with_state([](const ClusterState& state, const ClusterIndexes&) {
            return evaluate_readiness(state);
          });
        }
        send_frame(*session, MessageType::RevalidateResponse, ++sequence,
                   encode_revalidate_response(response));
        break;
      }
      case MessageType::Shutdown: {
        ShutdownMessage message;
        const DecodeOutcome decoded = decode_shutdown(payload, message);
        if (decoded.ok()) {
          session->stop.store(true);
        }
        break;
      }
      case MessageType::HelloAck:
      case MessageType::RegisterAck:
      case MessageType::PublishResult:
      case MessageType::HeartbeatAck:
      case MessageType::SnapshotResponse:
      case MessageType::ValidateSnapshotResponse:
      case MessageType::RevalidateResponse:
      case MessageType::Error:
      case MessageType::Invalid:
      default: {
        ErrorMessage error;
        error.error = StructuredError::make(ErrorCategory::Protocol, ErrorStage::Decode,
                                            "unknown_message",
                                            "message type is not accepted by the coordinator");
        send_frame(*session, MessageType::Error, ++sequence, encode_error(error));
        break;
      }
    }
  }

  detach_session(session->id);
  detail::shutdown_socket(session->handle);
  detail::close_socket(session->handle);
  session->handle = detail::kInvalidSocket;
}

void ClusterCoordinator::Impl::stop() {
  // Stop accepting mutations immediately: submit() must never wait for a
  // commit thread that is about to exit. The shutdown flag is published under
  // the queue lock first so a racing submit() reports SHUTTING_DOWN rather than
  // waiting for a result.
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    stopping = true;
  }
  running.store(false);
  if (accepting.exchange(false)) {
    if (listener != detail::kInvalidSocket) {
      detail::shutdown_socket(listener);
      detail::close_socket(listener);
      listener = detail::kInvalidSocket;
    }
  }
  if (accept_thread.joinable()) {
    accept_thread.join();
  }

  close_all_sessions();
  {
    std::unique_lock<std::mutex> lock(sessions_mutex);
    // Every session thread removes itself from the registry before exiting,
    // so an empty registry means no worker can touch this object any more.
    while (!sessions.empty()) {
      std::vector<std::shared_ptr<detail::Session>> snapshot;
      for (const auto& entry : sessions) {
        snapshot.push_back(entry.second);
      }
      lock.unlock();
      for (const std::shared_ptr<detail::Session>& session : snapshot) {
        session->stop.store(true);
        detail::shutdown_socket(session->handle);
      }
      lock.lock();
      if (sessions.empty()) {
        break;
      }
      // Bounded wait: each session loop observes its stop flag within one
      // poll interval.
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    stopping = true;
    detail::QueueItem item;
    item.kind = detail::QueueItem::Kind::Stop;
    queue.push_back(std::move(item));
  }
  queue_condition.notify_all();
  if (commit_thread.joinable()) {
    commit_thread.join();
  }

  std::deque<detail::QueueItem> pending;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    pending.swap(queue);
  }
  for (detail::QueueItem& item : pending) {
    if (item.slot == nullptr) {
      continue;
    }
    std::lock_guard<std::mutex> slot_lock(item.slot->mutex);
    item.slot->result = MutationResult::rejected(
        RejectionReason::ShuttingDown, ErrorStage::Commit, item.request.cluster.value(),
        "coordinator stopped before the mutation was processed", "shutting_down");
    item.slot->done = true;
    item.slot->condition.notify_all();
  }

  if (listener != detail::kInvalidSocket) {
    detail::close_socket(listener);
    listener = detail::kInvalidSocket;
  }
  if (store != nullptr) {
    store->discard_temporary();
  }
  bound_port = 0;
}

}  // namespace cluster_fabric
