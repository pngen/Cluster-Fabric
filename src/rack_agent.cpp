// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Rack-side publisher: transport, handshake, registration and the bounded
// mutation flow one rack agent drives against a single cluster coordinator.
//
// Every entry point is synchronous and single threaded: one request frame, one
// response frame, no watchdog and no background thread. The transport reuses
// the coordinator's own framing primitives and a short select() poll exists
// only so a blocked read can observe the internal stop flag. Failures are
// never reduced to a boolean or an exception: each one is recorded as a typed
// RackAgentStatus and returned as a typed MutationResult.

#include "cluster_fabric/rack_agent.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
#endif

namespace cluster_fabric {

std::string_view to_string(RackAgentState value) noexcept {
  switch (value) {
    case RackAgentState::Idle: return "IDLE";
    case RackAgentState::Connecting: return "CONNECTING";
    case RackAgentState::Handshaking: return "HANDSHAKING";
    case RackAgentState::Registering: return "REGISTERING";
    case RackAgentState::Publishing: return "PUBLISHING";
    case RackAgentState::Ready: return "READY";
    case RackAgentState::Failed: return "FAILED";
    case RackAgentState::Stopped: return "STOPPED";
  }
  return "UNKNOWN";
}

namespace {

enum class TransferResult { Ok, Closed, Error, Stopped };

/// Waits until the handle is readable. The poll interval exists only so a
/// blocked read can observe the stop flag; it is not an operation timeout.
[[nodiscard]] bool wait_readable(detail::socket_handle handle) noexcept {
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(handle, &read_set);
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
                                           const std::atomic<bool>& stop) noexcept {
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
    const int chunk =
        ::send(handle, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
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

/// Deterministic single-line rendering of a typed mutation result.
[[nodiscard]] std::string describe_result(const MutationResult& result) {
  if (!result.error.ok()) {
    const std::string text = result.error.describe();
    if (!text.empty()) {
      return text;
    }
  }
  if (!result.explanation.summary.empty()) {
    return result.explanation.summary;
  }
  return std::string(to_string(result.reason));
}

}  // namespace

struct RackAgent::Impl {
  Impl(RackAgentConfig agent_config, const Clock* agent_clock);

  /// Configuration as supplied, with the generated incarnation filled in.
  RackAgentConfig config;
  const Clock* clock = nullptr;
  RackAgentBootId boot;
  RackGeneration generation;
  /// Monotonic per-publisher counter. Consumed once per outbound frame.
  PublicationGeneration publication = PublicationGeneration::from_raw(1);
  RackAgentStatus status;
  /// Latest cluster lifecycle reported by the coordinator.
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;
  std::optional<std::string> snapshot_digest;

  detail::socket_handle handle = detail::kInvalidSocket;
  std::uint64_t sequence = 0;
  std::atomic<bool> stopping{false};
  bool registered = false;
  bool stopped = false;

  [[nodiscard]] Timestamp now() const;
  [[nodiscard]] std::string subject() const { return "rack/" + config.rack.value(); }
  /// Returns the current counter value and advances it. The counter saturates
  /// at its maximum instead of wrapping, so a send that cannot advance is
  /// rejected by the coordinator as a stale publication rather than replaying
  /// an older authority.
  [[nodiscard]] PublicationGeneration take_publication() noexcept;
  [[nodiscard]] MutationAuthority make_authority(PublicationGeneration used) const;
  [[nodiscard]] RackReference make_reference(PublicationGeneration used,
                                             Timestamp observed_at) const;
  [[nodiscard]] MutationRequest make_request(MutationKind kind, PublicationGeneration used,
                                             Timestamp observed_at) const;

  [[nodiscard]] bool transport_ready() const noexcept;
  [[nodiscard]] bool bounds_ok(std::string& detail) const;
  void record_failure(ProtocolStatus transport, RejectionReason reason, std::string detail);
  void record_rejection(RejectionReason reason, std::string detail);
  void clear_rejection() noexcept;
  [[nodiscard]] MutationResult transport_result(ErrorStage stage) const;

  [[nodiscard]] bool send_frame(MessageType type, const std::string& payload);
  [[nodiscard]] bool read_frame(MessageType expected, FrameOutcome& out);
  [[nodiscard]] bool submit_request(const MutationRequest& request, MutationResult& out);
  void observe(const MutationResult& result);
  [[nodiscard]] bool publish_topology(Timestamp observed_at);

  bool connect();
  [[nodiscard]] MutationResult publish();
  [[nodiscard]] MutationResult supersede(RackGeneration new_generation);
  [[nodiscard]] MutationResult revalidate();
  [[nodiscard]] MutationResult heartbeat();
  [[nodiscard]] SnapshotResponseMessage request_snapshot();
  [[nodiscard]] ValidateSnapshotResponseMessage validate_snapshot(const SnapshotView& view);
  void stop() noexcept;
};

RackAgent::Impl::Impl(RackAgentConfig agent_config, const Clock* agent_clock)
    : config(std::move(agent_config)), clock(agent_clock) {
  if (!config.boot.has_value()) {
    config.boot = make_rack_agent_boot_id("rack-agent");
  }
  boot = *config.boot;
  generation = config.generation;
  publication = PublicationGeneration::from_raw(1);
  status.state = RackAgentState::Idle;
  status.transport = ProtocolStatus::Ok;
  status.rejection = RejectionReason::None;
  status.publication = publication;
}

Timestamp RackAgent::Impl::now() const {
  const Clock& source = clock != nullptr ? *clock : default_clock();
  return source.now();
}

PublicationGeneration RackAgent::Impl::take_publication() noexcept {
  const PublicationGeneration current = publication;
  const std::optional<PublicationGeneration> next = publication.next();
  if (next.has_value()) {
    publication = *next;
  }
  return current;
}

MutationAuthority RackAgent::Impl::make_authority(PublicationGeneration used) const {
  MutationAuthority authority;
  authority.cluster_epoch = status.cluster_epoch;
  authority.coordinator_epoch = status.coordinator_epoch;
  authority.topology_epoch = status.topology_epoch;
  authority.topology_generation = status.topology_generation;
  authority.boot = boot;
  authority.publisher = config.publisher;
  authority.rack = config.rack;
  authority.rack_generation = generation;
  authority.publication = used;
  return authority;
}

RackReference RackAgent::Impl::make_reference(PublicationGeneration used,
                                              Timestamp observed_at) const {
  RackReference reference;
  reference.rack = config.rack;
  reference.generation = generation;
  reference.rack_lifecycle = config.rack_lifecycle;
  reference.currentness = RackCurrentness::Current;
  reference.composition = config.composition;
  reference.endpoints = config.endpoints;
  reference.failure_domain_hints = config.failure_domain_hints;
  reference.evidence =
      EvidenceStamp::make(EvidenceProvenance::Reported, observed_at, config.publication_ttl_millis);
  reference.publisher = config.publisher;
  reference.publication = used;
  reference.boot = boot;
  reference.cluster_epoch = status.cluster_epoch;
  reference.coordinator_epoch = status.coordinator_epoch;
  reference.health = config.health;
  reference.origin_label = config.origin_label;
  // Canonical order keeps the encoded publication byte-identical for identical
  // inputs, which is what makes idempotent republication detectable.
  return canonicalize(std::move(reference));
}

MutationRequest RackAgent::Impl::make_request(MutationKind kind, PublicationGeneration used,
                                              Timestamp observed_at) const {
  MutationRequest request;
  request.kind = kind;
  request.cluster = config.cluster;
  request.authority = make_authority(used);
  request.evidence =
      EvidenceStamp::make(EvidenceProvenance::Reported, observed_at, config.publication_ttl_millis);
  request.reason = "rack_agent_publication";
  request.requested_at = observed_at;
  request.rack_reference = make_reference(used, observed_at);
  return request;
}

bool RackAgent::Impl::transport_ready() const noexcept {
  return handle != detail::kInvalidSocket && !stopped;
}

bool RackAgent::Impl::bounds_ok(std::string& detail) const {
  if (config.endpoints.size() > kMaxRackEndpoints) {
    detail = "configured endpoint count " + std::to_string(config.endpoints.size()) +
             " exceeds " + std::to_string(kMaxRackEndpoints);
    return false;
  }
  if (config.failure_domain_hints.size() > kMaxFailureDomainRefs) {
    detail = "configured failure-domain hint count " +
             std::to_string(config.failure_domain_hints.size()) + " exceeds " +
             std::to_string(kMaxFailureDomainRefs);
    return false;
  }
  if (config.composition.accelerators.size() > kMaxCapacityQuantities) {
    detail = "configured accelerator class count " +
             std::to_string(config.composition.accelerators.size()) + " exceeds " +
             std::to_string(kMaxCapacityQuantities);
    return false;
  }
  if (config.links.size() > kMaxInterRackLinks) {
    detail = "configured link count " + std::to_string(config.links.size()) + " exceeds " +
             std::to_string(kMaxInterRackLinks);
    return false;
  }
  if (config.failure_domains.size() > kMaxDomainsPerClass) {
    detail = "configured failure-domain count " + std::to_string(config.failure_domains.size()) +
             " exceeds " + std::to_string(kMaxDomainsPerClass);
    return false;
  }
  if (config.placement_domains.size() > kMaxDomainsPerClass) {
    detail = "configured placement-domain count " +
             std::to_string(config.placement_domains.size()) + " exceeds " +
             std::to_string(kMaxDomainsPerClass);
    return false;
  }
  return true;
}

void RackAgent::Impl::record_failure(ProtocolStatus transport, RejectionReason reason,
                                     std::string detail) {
  status.state = RackAgentState::Failed;
  status.transport = transport;
  status.rejection = reason;
  status.detail = std::move(detail);
  if (reason == RejectionReason::StaleRackBoot) {
    status.fenced = true;
  }
}

void RackAgent::Impl::record_rejection(RejectionReason reason, std::string detail) {
  status.rejection = reason;
  status.detail = std::move(detail);
  if (reason == RejectionReason::StaleRackBoot) {
    status.fenced = true;
  }
}

void RackAgent::Impl::clear_rejection() noexcept {
  status.rejection = RejectionReason::None;
  status.detail.clear();
}

MutationResult RackAgent::Impl::transport_result(ErrorStage stage) const {
  const RejectionReason reason = status.transport == ProtocolStatus::NotConnected
                                     ? RejectionReason::NotReady
                                     : RejectionReason::Internal;
  const std::string code =
      status.transport == ProtocolStatus::NotConnected ? "not_connected" : "transport_failure";
  return MutationResult::rejected(reason, stage, subject(), status.detail, code);
}

bool RackAgent::Impl::send_frame(MessageType type, const std::string& payload) {
  if (!transport_ready()) {
    record_failure(ProtocolStatus::NotConnected, RejectionReason::NotReady,
                   "transport is not connected");
    return false;
  }
  const std::string frame = encode_frame(type, ++sequence, payload);
  if (frame.empty()) {
    record_failure(ProtocolStatus::OversizedFrame, RejectionReason::Internal,
                   "frame could not be encoded within the configured payload bound");
    return false;
  }
  if (!send_all(handle, frame)) {
    record_failure(ProtocolStatus::IoError, RejectionReason::Internal,
                   std::string("send failed: ") + detail::socket_error_text());
    return false;
  }
  return true;
}

bool RackAgent::Impl::read_frame(MessageType expected, FrameOutcome& out) {
  char header_bytes[kFrameHeaderSize];
  const TransferResult header_result =
      receive_exact(handle, header_bytes, sizeof(header_bytes), stopping);
  if (header_result == TransferResult::Stopped) {
    record_failure(ProtocolStatus::NotConnected, RejectionReason::NotReady,
                   "transport was stopped while waiting for a response");
    return false;
  }
  if (header_result != TransferResult::Ok) {
    record_failure(ProtocolStatus::ConnectionClosed, RejectionReason::Internal,
                   std::string("the coordinator closed the session: ") +
                       detail::socket_error_text());
    return false;
  }
  FrameOutcome frame = decode_frame(std::string_view(header_bytes, sizeof(header_bytes)));
  if (frame.status == ProtocolStatus::TruncatedPayload) {
    const std::uint32_t payload_length = frame.header.payload_length;
    if (payload_length > kMaxFramePayloadBytes) {
      record_failure(ProtocolStatus::OversizedFrame, RejectionReason::Malformed,
                     "declared payload exceeds the configured maximum");
      return false;
    }
    std::string buffer(header_bytes, sizeof(header_bytes));
    buffer.resize(kFrameHeaderSize + static_cast<std::size_t>(payload_length));
    const TransferResult body_result = receive_exact(
        handle, buffer.data() + kFrameHeaderSize, static_cast<std::size_t>(payload_length),
        stopping);
    if (body_result != TransferResult::Ok) {
      record_failure(ProtocolStatus::TruncatedPayload, RejectionReason::Internal,
                     "the response payload did not arrive in full");
      return false;
    }
    frame = decode_frame(buffer);
  }
  if (!frame.ok()) {
    record_failure(frame.status, RejectionReason::Malformed,
                   frame.error.message.empty() ? std::string(to_string(frame.status))
                                               : frame.error.message);
    return false;
  }
  if (frame.header.type == MessageType::Error) {
    ErrorMessage error;
    const DecodeOutcome decoded = decode_error(frame.payload, error);
    record_failure(ProtocolStatus::Rejected, RejectionReason::Malformed,
                   decoded.ok() ? error.error.describe() : std::string("peer reported an error"));
    return false;
  }
  if (frame.header.type != expected) {
    record_failure(ProtocolStatus::UnknownMessage, RejectionReason::Malformed,
                   std::string("expected ") + std::string(to_string(expected)) +
                       " but received " + std::string(to_string(frame.header.type)));
    return false;
  }
  out = std::move(frame);
  return true;
}

bool RackAgent::Impl::submit_request(const MutationRequest& request, MutationResult& out) {
  PublishRequestMessage message;
  message.request = request;
  const std::string payload = encode_publish_request(message);
  if (payload.empty()) {
    record_failure(ProtocolStatus::OversizedFrame, RejectionReason::Internal,
                   "publish request could not be encoded within the configured bound");
    out = transport_result(ErrorStage::Encode);
    return false;
  }
  if (!send_frame(MessageType::PublishRequest, payload)) {
    out = transport_result(ErrorStage::Transport);
    return false;
  }
  FrameOutcome frame;
  if (!read_frame(MessageType::PublishResult, frame)) {
    out = transport_result(ErrorStage::Transport);
    return false;
  }
  PublishResultMessage response;
  const DecodeOutcome decoded = decode_publish_result(frame.payload, response);
  if (!decoded.ok()) {
    record_failure(ProtocolStatus::MalformedPayload, RejectionReason::Malformed,
                   decoded.error.message.empty() ? std::string("publish result is malformed")
                                                 : decoded.error.message);
    out = transport_result(ErrorStage::Decode);
    return false;
  }
  out = std::move(response.result);
  return true;
}

void RackAgent::Impl::observe(const MutationResult& result) {
  if (result.cluster_epoch.known()) {
    status.cluster_epoch = result.cluster_epoch;
  }
  if (result.coordinator_epoch.known()) {
    status.coordinator_epoch = result.coordinator_epoch;
  }
  if (result.topology_epoch.known()) {
    status.topology_epoch = result.topology_epoch;
  }
  if (result.topology_generation.known()) {
    status.topology_generation = result.topology_generation;
  }
  lifecycle = result.lifecycle;
}

bool RackAgent::Impl::publish_topology(Timestamp observed_at) {
  bool transport_ok = true;
  bool recorded = false;

  const auto attempt = [this, observed_at, &transport_ok, &recorded](MutationRequest request) {
    MutationResult result;
    if (!submit_request(request, result)) {
      transport_ok = false;
      return;
    }
    if (result.accepted()) {
      observe(result);
      return;
    }
    if (!recorded) {
      recorded = true;
      record_rejection(result.reason, describe_result(result));
    }
  };

  const auto stamp_evidence = [this, observed_at]() {
    return EvidenceStamp::make(EvidenceProvenance::Reported, observed_at,
                               config.publication_ttl_millis);
  };

  // Configured order: inter-rack links, then failure domains, then placement
  // domains. Only link payloads carry a topology epoch; every payload that has
  // a domain header receives the latest authoritative cluster and coordinator
  // epochs.
  for (const InterRackLink& configured : config.links) {
    if (!transport_ok) {
      break;
    }
    const PublicationGeneration used = take_publication();
    status.publication = used;
    MutationRequest request = make_request(MutationKind::PublishInterRackLink, used, observed_at);
    request.reason = "rack_agent_topology_publication";
    request.topology_reason = "rack_agent_topology_publication";
    InterRackLink link = configured;
    link.topology_epoch = status.topology_epoch;
    link.topology_generation = status.topology_generation;
    link.header.cluster_epoch = status.cluster_epoch;
    link.header.coordinator_epoch = status.coordinator_epoch;
    link.header.evidence = stamp_evidence();
    link.header.publisher = config.publisher;
    link.publication = used;
    link.boot = boot;
    request.link = std::move(link);
    attempt(std::move(request));
  }

  for (const FailureDomain& configured : config.failure_domains) {
    if (!transport_ok) {
      break;
    }
    const PublicationGeneration used = take_publication();
    status.publication = used;
    MutationRequest request = make_request(MutationKind::PublishFailureDomain, used, observed_at);
    request.reason = "rack_agent_topology_publication";
    FailureDomain domain = configured;
    domain.header.cluster_epoch = status.cluster_epoch;
    domain.header.coordinator_epoch = status.coordinator_epoch;
    domain.header.evidence = stamp_evidence();
    domain.header.publisher = config.publisher;
    request.failure_domain = std::move(domain);
    attempt(std::move(request));
  }

  for (const PlacementDomain& configured : config.placement_domains) {
    if (!transport_ok) {
      break;
    }
    const PublicationGeneration used = take_publication();
    status.publication = used;
    MutationRequest request = make_request(MutationKind::PublishPlacementDomain, used, observed_at);
    request.reason = "rack_agent_topology_publication";
    PlacementDomain domain = configured;
    domain.header.cluster_epoch = status.cluster_epoch;
    domain.header.coordinator_epoch = status.coordinator_epoch;
    domain.header.evidence = stamp_evidence();
    domain.header.publisher = config.publisher;
    request.placement_domain = std::move(domain);
    attempt(std::move(request));
  }

  return transport_ok;
}

bool RackAgent::Impl::connect() {
  if (stopped) {
    record_failure(ProtocolStatus::NotRunning, RejectionReason::NotReady,
                   "rack agent transport is stopped");
    return false;
  }
  if (registered && handle != detail::kInvalidSocket) {
    return true;
  }
  if (handle != detail::kInvalidSocket) {
    detail::shutdown_socket(handle);
    detail::close_socket(handle);
    handle = detail::kInvalidSocket;
  }
  registered = false;
  status.registered = false;
  status.connected = false;
  status.state = RackAgentState::Connecting;
  status.transport = ProtocolStatus::Ok;
  clear_rejection();

  if (!detail::socket_library_init()) {
    record_failure(ProtocolStatus::IoError, RejectionReason::Internal,
                   "socket library initialization failed");
    return false;
  }
  const detail::socket_handle created =
      static_cast<detail::socket_handle>(::socket(AF_INET, SOCK_STREAM, 0));
  if (created == detail::kInvalidSocket) {
    record_failure(ProtocolStatus::IoError, RejectionReason::Internal,
                   std::string("could not create a TCP socket: ") + detail::socket_error_text());
    return false;
  }
  handle = created;

  sockaddr_in address;
  std::memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(config.coordinator_port);
  if (::inet_pton(AF_INET, config.coordinator_host.c_str(), &address.sin_addr) != 1) {
    record_failure(ProtocolStatus::IoError, RejectionReason::Internal,
                   "coordinator address is not a valid IPv4 address: " + config.coordinator_host);
    detail::shutdown_socket(handle);
    detail::close_socket(handle);
    handle = detail::kInvalidSocket;
    return false;
  }
  if (::connect(handle, reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address))) != 0) {
    record_failure(ProtocolStatus::NotConnected, RejectionReason::Internal,
                   "connect to " + config.coordinator_host + ":" +
                       std::to_string(config.coordinator_port) +
                       " failed: " + detail::socket_error_text());
    detail::shutdown_socket(handle);
    detail::close_socket(handle);
    handle = detail::kInvalidSocket;
    return false;
  }
  status.connected = true;
  status.state = RackAgentState::Handshaking;

  HelloMessage hello;
  hello.protocol_version = kProtocolVersion;
  hello.cluster = config.cluster;
  hello.boot = boot;
  hello.role = "rack_agent";
  hello.build = std::string(version_banner());
  const std::string hello_payload = encode_hello(hello);
  if (hello_payload.empty()) {
    record_failure(ProtocolStatus::OversizedFrame, RejectionReason::Internal,
                   "hello payload could not be encoded within the configured bound");
    return false;
  }
  if (!send_frame(MessageType::Hello, hello_payload)) {
    return false;
  }
  FrameOutcome frame;
  if (!read_frame(MessageType::HelloAck, frame)) {
    return false;
  }
  HelloAckMessage ack;
  const DecodeOutcome decoded = decode_hello_ack(frame.payload, ack);
  if (!decoded.ok()) {
    record_failure(ProtocolStatus::MalformedPayload, RejectionReason::Malformed,
                   decoded.error.message.empty() ? std::string("hello ack is malformed")
                                                 : decoded.error.message);
    return false;
  }
  if (!ack.accepted) {
    record_failure(ProtocolStatus::Rejected, ack.reason, ack.detail);
    return false;
  }
  if (ack.protocol_version != kProtocolVersion) {
    record_failure(ProtocolStatus::UnsupportedVersion, RejectionReason::Malformed,
                   "coordinator reported protocol version " +
                       std::to_string(ack.protocol_version));
    return false;
  }
  if (ack.cluster != config.cluster) {
    record_failure(ProtocolStatus::Rejected, RejectionReason::WrongCluster,
                   "coordinator accepted a different cluster: " + ack.cluster.value());
    return false;
  }
  if (ack.coordinator_epoch.known()) {
    status.coordinator_epoch = ack.coordinator_epoch;
  }
  if (ack.cluster_epoch.known()) {
    status.cluster_epoch = ack.cluster_epoch;
  }
  if (ack.topology_epoch.known()) {
    status.topology_epoch = ack.topology_epoch;
  }
  if (ack.topology_generation.known()) {
    status.topology_generation = ack.topology_generation;
  }
  lifecycle = ack.lifecycle;
  status.state = RackAgentState::Registering;

  const PublicationGeneration used = take_publication();
  status.publication = used;
  RegisterPublisherMessage registration;
  registration.cluster = config.cluster;
  registration.authority = make_authority(used);
  registration.publisher = config.publisher;
  registration.rack = config.rack;
  registration.generation = generation;
  registration.rack_lifecycle = config.rack_lifecycle;
  registration.composition = config.composition;
  registration.origin_label = config.origin_label;
  registration.sent_at = now();
  const std::string register_payload = encode_register_publisher(registration);
  if (register_payload.empty()) {
    record_failure(ProtocolStatus::OversizedFrame, RejectionReason::Internal,
                   "registration payload could not be encoded within the configured bound");
    return false;
  }
  if (!send_frame(MessageType::RegisterPublisher, register_payload)) {
    return false;
  }
  if (!read_frame(MessageType::RegisterAck, frame)) {
    return false;
  }
  RegisterAckMessage register_ack;
  const DecodeOutcome register_decoded = decode_register_ack(frame.payload, register_ack);
  if (!register_decoded.ok()) {
    record_failure(ProtocolStatus::MalformedPayload, RejectionReason::Malformed,
                   register_decoded.error.message.empty()
                       ? std::string("registration ack is malformed")
                       : register_decoded.error.message);
    return false;
  }
  if (!register_ack.accepted) {
    record_failure(ProtocolStatus::Rejected, register_ack.reason, register_ack.detail);
    return false;
  }
  if (register_ack.coordinator_epoch.known()) {
    status.coordinator_epoch = register_ack.coordinator_epoch;
  }
  if (register_ack.cluster_epoch.known()) {
    status.cluster_epoch = register_ack.cluster_epoch;
  }
  if (register_ack.topology_epoch.known()) {
    status.topology_epoch = register_ack.topology_epoch;
  }
  if (register_ack.topology_generation.known()) {
    status.topology_generation = register_ack.topology_generation;
  }
  registered = true;
  status.registered = true;
  status.connected = true;
  status.state = RackAgentState::Ready;
  status.transport = ProtocolStatus::Ok;
  clear_rejection();
  return true;
}

MutationResult RackAgent::Impl::publish() {
  if (!transport_ready()) {
    record_failure(ProtocolStatus::NotConnected, RejectionReason::NotReady,
                   "transport is not connected");
    return transport_result(ErrorStage::Transport);
  }
  std::string bound_detail;
  if (!bounds_ok(bound_detail)) {
    record_rejection(RejectionReason::LimitExceeded, bound_detail);
    return MutationResult::rejected(RejectionReason::LimitExceeded, ErrorStage::ValidateReference,
                                    subject(), bound_detail, "limit_exceeded");
  }

  status.state = RackAgentState::Publishing;
  const Timestamp observed_at = now();
  const PublicationGeneration used = take_publication();
  status.publication = used;
  MutationRequest request = make_request(MutationKind::AddRack, used, observed_at);
  request.reason = "rack_agent_publication";

  MutationResult result;
  if (!submit_request(request, result)) {
    return result;
  }
  if (!result.accepted()) {
    record_rejection(result.reason, describe_result(result));
    status.state = registered ? RackAgentState::Ready : status.state;
    return result;
  }
  observe(result);
  clear_rejection();

  const bool has_topology = !config.links.empty() || !config.failure_domains.empty() ||
                            !config.placement_domains.empty();
  if (registered && has_topology) {
    // A transport failure during a topology publication is a transport
    // failure; a rejection is recorded as a typed status and the rack result
    // is still returned to the caller.
    const bool topology_ok = publish_topology(observed_at);
    if (!topology_ok) {
      status.state = RackAgentState::Failed;
      return result;
    }
  }
  status.state = RackAgentState::Ready;
  return result;
}

MutationResult RackAgent::Impl::supersede(RackGeneration new_generation) {
  if (!transport_ready()) {
    record_failure(ProtocolStatus::NotConnected, RejectionReason::NotReady,
                   "transport is not connected");
    return transport_result(ErrorStage::Transport);
  }
  status.state = RackAgentState::Publishing;
  const Timestamp observed_at = now();
  const PublicationGeneration used = take_publication();
  status.publication = used;
  MutationRequest request = make_request(MutationKind::UpdateRackGeneration, used, observed_at);
  request.reason = "rack_agent_supersede";
  request.expected_rack_generation = generation;
  request.new_rack_generation = new_generation;

  MutationResult result;
  if (!submit_request(request, result)) {
    return result;
  }
  if (!result.accepted()) {
    record_rejection(result.reason, describe_result(result));
    status.state = registered ? RackAgentState::Ready : status.state;
    return result;
  }
  observe(result);
  clear_rejection();
  generation = new_generation;
  // The update result is returned; the republish outcome is folded into the
  // typed status so a failed republication is never silent.
  const MutationResult republished = publish();
  if (!republished.accepted()) {
    record_rejection(republished.reason, describe_result(republished));
  }
  return result;
}

MutationResult RackAgent::Impl::revalidate() {
  if (!transport_ready()) {
    record_failure(ProtocolStatus::NotConnected, RejectionReason::NotReady,
                   "transport is not connected");
    return transport_result(ErrorStage::Transport);
  }
  status.state = RackAgentState::Publishing;
  const Timestamp observed_at = now();
  const PublicationGeneration used = take_publication();
  status.publication = used;
  const MutationRequest request = make_request(MutationKind::RevalidateRecoveredState, used,
                                               observed_at);

  RevalidateRequestMessage message;
  message.cluster = config.cluster;
  message.authority = request.authority;
  // One rack identity is always within kMaxRacksPerCluster.
  message.racks.push_back(config.rack);
  const std::string payload = encode_revalidate_request(message);
  if (payload.empty()) {
    record_failure(ProtocolStatus::OversizedFrame, RejectionReason::Internal,
                   "revalidation request could not be encoded within the configured bound");
    return transport_result(ErrorStage::Encode);
  }
  if (!send_frame(MessageType::RevalidateRequest, payload)) {
    return transport_result(ErrorStage::Transport);
  }
  FrameOutcome frame;
  if (!read_frame(MessageType::RevalidateResponse, frame)) {
    return transport_result(ErrorStage::Transport);
  }
  RevalidateResponseMessage response;
  const DecodeOutcome decoded = decode_revalidate_response(frame.payload, response);
  if (!decoded.ok()) {
    record_failure(ProtocolStatus::MalformedPayload, RejectionReason::Malformed,
                   decoded.error.message.empty() ? std::string("revalidation response is malformed")
                                                 : decoded.error.message);
    return transport_result(ErrorStage::Decode);
  }
  if (!response.accepted) {
    record_rejection(response.reason, response.detail);
    status.state = registered ? RackAgentState::Ready : status.state;
    return MutationResult::rejected(response.reason, ErrorStage::ValidateAuthority, subject(),
                                    response.detail, "revalidation_refused");
  }

  // The revalidation response carries no epochs, so authority is refreshed
  // from the republish that follows it.
  lifecycle = response.lifecycle;
  status.state = RackAgentState::Publishing;
  MutationResult result;
  result.outcome = MutationOutcome::Accepted;
  result.reason = RejectionReason::None;
  result.lifecycle = response.lifecycle;
  result.cluster_epoch = status.cluster_epoch;
  result.coordinator_epoch = status.coordinator_epoch;
  result.topology_epoch = status.topology_epoch;
  result.topology_generation = status.topology_generation;
  result.publication_generation = status.publication;
  result.explanation = Explanation::make("revalidation_accepted", subject(),
                                         response.detail.empty()
                                             ? std::string("recovered state revalidated")
                                             : response.detail);
  result.explanation.add("revalidation_accepted", subject(),
                         "rack evidence is current under the observed authority");
  result.explanation.sort_factors();
  result.explanation.bound_factors();
  const MutationResult republished = publish();
  if (!republished.accepted()) {
    record_rejection(republished.reason, describe_result(republished));
  }
  return result;
}

MutationResult RackAgent::Impl::heartbeat() {
  if (!transport_ready()) {
    record_failure(ProtocolStatus::NotConnected, RejectionReason::NotReady,
                   "transport is not connected");
    return transport_result(ErrorStage::Transport);
  }
  const PublicationGeneration used = take_publication();
  status.publication = used;
  HeartbeatMessage message;
  message.cluster = config.cluster;
  message.authority = make_authority(used);
  message.sent_at = now();
  const std::string payload = encode_heartbeat(message);
  if (payload.empty()) {
    record_failure(ProtocolStatus::OversizedFrame, RejectionReason::Internal,
                   "heartbeat payload could not be encoded within the configured bound");
    return transport_result(ErrorStage::Encode);
  }
  if (!send_frame(MessageType::Heartbeat, payload)) {
    return transport_result(ErrorStage::Transport);
  }
  FrameOutcome frame;
  if (!read_frame(MessageType::HeartbeatAck, frame)) {
    return transport_result(ErrorStage::Transport);
  }
  HeartbeatAckMessage ack;
  const DecodeOutcome decoded = decode_heartbeat_ack(frame.payload, ack);
  if (!decoded.ok()) {
    record_failure(ProtocolStatus::MalformedPayload, RejectionReason::Malformed,
                   decoded.error.message.empty() ? std::string("heartbeat ack is malformed")
                                                 : decoded.error.message);
    return transport_result(ErrorStage::Decode);
  }
  if (!ack.accepted) {
    record_rejection(ack.reason, ack.detail);
    return MutationResult::rejected(ack.reason, ErrorStage::ValidateAuthority, subject(),
                                    ack.detail, "heartbeat_refused");
  }
  if (ack.coordinator_epoch.known()) {
    status.coordinator_epoch = ack.coordinator_epoch;
  }
  if (ack.cluster_epoch.known()) {
    status.cluster_epoch = ack.cluster_epoch;
  }
  if (ack.topology_epoch.known()) {
    status.topology_epoch = ack.topology_epoch;
  }
  lifecycle = ack.lifecycle;
  clear_rejection();

  MutationResult result;
  result.outcome = MutationOutcome::NoChange;
  result.reason = RejectionReason::None;
  result.lifecycle = ack.lifecycle;
  result.cluster_epoch = status.cluster_epoch;
  result.coordinator_epoch = status.coordinator_epoch;
  result.topology_epoch = status.topology_epoch;
  result.topology_generation = status.topology_generation;
  result.publication_generation = used;
  result.explanation = Explanation::make("heartbeat_accepted", subject(),
                                         ack.detail.empty()
                                             ? std::string("heartbeat accepted")
                                             : ack.detail);
  result.explanation.sort_factors();
  result.explanation.bound_factors();
  return result;
}

SnapshotResponseMessage RackAgent::Impl::request_snapshot() {
  SnapshotResponseMessage response;
  if (!transport_ready()) {
    record_failure(ProtocolStatus::NotConnected, RejectionReason::NotReady,
                   "transport is not connected");
    return response;
  }
  status.publication = take_publication();
  SnapshotRequestMessage message;
  message.cluster = config.cluster;
  const std::string payload = encode_snapshot_request(message);
  if (payload.empty()) {
    record_failure(ProtocolStatus::OversizedFrame, RejectionReason::Internal,
                   "snapshot request could not be encoded within the configured bound");
    return response;
  }
  if (!send_frame(MessageType::SnapshotRequest, payload)) {
    return response;
  }
  FrameOutcome frame;
  if (!read_frame(MessageType::SnapshotResponse, frame)) {
    return response;
  }
  const DecodeOutcome decoded = decode_snapshot_response(frame.payload, response);
  if (!decoded.ok()) {
    record_failure(ProtocolStatus::MalformedPayload, RejectionReason::Malformed,
                   decoded.error.message.empty() ? std::string("snapshot response is malformed")
                                                 : decoded.error.message);
    return SnapshotResponseMessage{};
  }
  if (response.accepted) {
    snapshot_digest = response.view.semantic_digest;
    clear_rejection();
  } else {
    record_rejection(response.reason, response.detail);
  }
  return response;
}

ValidateSnapshotResponseMessage RackAgent::Impl::validate_snapshot(const SnapshotView& view) {
  ValidateSnapshotResponseMessage response;
  if (!transport_ready()) {
    record_failure(ProtocolStatus::NotConnected, RejectionReason::NotReady,
                   "transport is not connected");
    return response;
  }
  status.publication = take_publication();
  ValidateSnapshotRequestMessage message;
  message.view = view;
  const std::string payload = encode_validate_request(message);
  if (payload.empty()) {
    record_failure(ProtocolStatus::OversizedFrame, RejectionReason::Internal,
                   "validate request could not be encoded within the configured bound");
    return response;
  }
  if (!send_frame(MessageType::ValidateSnapshotRequest, payload)) {
    return response;
  }
  FrameOutcome frame;
  if (!read_frame(MessageType::ValidateSnapshotResponse, frame)) {
    return response;
  }
  const DecodeOutcome decoded = decode_validate_response(frame.payload, response);
  if (!decoded.ok()) {
    record_failure(ProtocolStatus::MalformedPayload, RejectionReason::Malformed,
                   decoded.error.message.empty() ? std::string("validation response is malformed")
                                                 : decoded.error.message);
    return ValidateSnapshotResponseMessage{};
  }
  return response;
}

void RackAgent::Impl::stop() noexcept {
  if (stopped) {
    return;
  }
  stopped = true;
  stopping.store(true);
  if (handle != detail::kInvalidSocket) {
    // Best effort: the coordinator treats SHUTDOWN as an end-of-session
    // request and never answers it. A failure here is not reported because the
    // socket is closed immediately afterwards either way.
    try {
      ShutdownMessage message;
      message.reason = "rack_agent_stopping";
      const std::string payload = encode_shutdown(message);
      if (!payload.empty()) {
        const std::string frame = encode_frame(MessageType::Shutdown, ++sequence, payload);
        if (!frame.empty()) {
          (void)send_all(handle, frame);
        }
      }
    } catch (...) {
      // The transport is being torn down; a failed best-effort write is not an
      // error the caller can act on.
    }
    detail::shutdown_socket(handle);
    detail::close_socket(handle);
    handle = detail::kInvalidSocket;
  }
  registered = false;
  status.registered = false;
  status.connected = false;
  status.state = RackAgentState::Stopped;
  status.transport = ProtocolStatus::NotRunning;
  status.detail = "transport stopped";
}

RackAgent::RackAgent(RackAgentConfig config, const Clock* clock)
    : impl_(std::make_unique<Impl>(std::move(config), clock)) {}

RackAgent::~RackAgent() {
  if (impl_ != nullptr) {
    impl_->stop();
  }
}

bool RackAgent::connect() { return impl_->connect(); }

MutationResult RackAgent::publish() { return impl_->publish(); }

MutationResult RackAgent::supersede(RackGeneration new_generation) {
  return impl_->supersede(new_generation);
}

MutationResult RackAgent::revalidate() { return impl_->revalidate(); }

MutationResult RackAgent::heartbeat() { return impl_->heartbeat(); }

SnapshotResponseMessage RackAgent::request_snapshot() { return impl_->request_snapshot(); }

ValidateSnapshotResponseMessage RackAgent::validate_snapshot(const SnapshotView& view) {
  return impl_->validate_snapshot(view);
}

void RackAgent::stop() { impl_->stop(); }

const RackAgentBootId& RackAgent::boot() const noexcept { return impl_->boot; }

const RackAgentStatus& RackAgent::status() const noexcept { return impl_->status; }

RackGeneration RackAgent::generation() const noexcept { return impl_->generation; }

std::optional<std::string> RackAgent::last_snapshot_digest() const {
  return impl_->snapshot_digest;
}

}  // namespace cluster_fabric
