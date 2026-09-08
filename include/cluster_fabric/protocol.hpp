// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Bounded framed protocol between the cluster coordinator and rack-side
// publishers.
//
// Frame layout (little-endian):
//
//   offset 0   u32  magic          'C','P','F','1'
//   offset 4   u16  protocol version
//   offset 6   u16  message type
//   offset 8   u32  flags
//   offset 12  u64  sequence
//   offset 20  u32  payload length
//   offset 24  u32  CRC32 over bytes [0,24) followed by the payload
//   offset 28  ...  payload
//
// Every length is checked before a byte is read or allocated. A frame that
// exceeds the configured maximum, declares a length it does not carry, or
// carries a bad checksum is rejected without touching canonical state.

#ifndef CLUSTER_FABRIC_PROTOCOL_HPP
#define CLUSTER_FABRIC_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cluster_fabric/error.hpp"
#include "cluster_fabric/limits.hpp"
#include "cluster_fabric/mutation.hpp"
#include "cluster_fabric/snapshot.hpp"
#include "cluster_fabric/version.hpp"

namespace cluster_fabric {

enum class ProtocolStatus : std::uint8_t {
  Ok = 0,
  BadMagic,
  UnsupportedVersion,
  TruncatedHeader,
  TruncatedPayload,
  OversizedFrame,
  ChecksumMismatch,
  UnknownMessage,
  MalformedPayload,
  AbsurdLength,
  TrailingGarbage,
  InvalidEnum,
  InvalidIdentity,
  ConnectionClosed,
  IoError,
  NotConnected,
  AlreadyRunning,
  NotRunning,
  ShutdownInProgress,
  Rejected,
};

[[nodiscard]] std::string_view to_string(ProtocolStatus value) noexcept;

enum class MessageType : std::uint16_t {
  Invalid = 0,
  Hello = 1,
  HelloAck = 2,
  RegisterPublisher = 3,
  RegisterAck = 4,
  PublishRequest = 5,
  PublishResult = 6,
  Heartbeat = 7,
  HeartbeatAck = 8,
  SnapshotRequest = 9,
  SnapshotResponse = 10,
  ValidateSnapshotRequest = 11,
  ValidateSnapshotResponse = 12,
  RevalidateRequest = 13,
  RevalidateResponse = 14,
  Shutdown = 15,
  Error = 16,
};

[[nodiscard]] std::string_view to_string(MessageType value) noexcept;
[[nodiscard]] std::optional<MessageType> message_type_from_string(std::string_view text) noexcept;

/// Bounded little-endian writer. Every length is checked before it is
/// written; the writer records the first failure and never throws.
class ByteWriter {
 public:
  explicit ByteWriter(std::size_t reserve = 256);

  void u8(std::uint8_t value);
  void boolean(bool value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void f64(double value);
  /// Length-prefixed bytes, bounded by kMaxEncodedStringBytes.
  void bytes(std::string_view value);
  /// Length-prefixed bounded text.
  void text(std::string_view value);
  /// Optional wrapper: 0 = absent, 1 = present.
  void optional_present(bool present);

  [[nodiscard]] bool ok() const noexcept { return status_ == ProtocolStatus::Ok; }
  [[nodiscard]] ProtocolStatus status() const noexcept { return status_; }
  [[nodiscard]] const std::string& data() const noexcept { return data_; }
  [[nodiscard]] std::string take() && { return std::move(data_); }

 private:
  void fail(ProtocolStatus status) noexcept {
    if (status_ == ProtocolStatus::Ok) {
      status_ = status;
    }
  }

  std::string data_;
  ProtocolStatus status_ = ProtocolStatus::Ok;
};

/// Bounded little-endian reader. Out-of-range reads fail and return nullopt.
class ByteReader {
 public:
  explicit ByteReader(std::string_view data) : data_(data) {}

  [[nodiscard]] std::optional<std::uint8_t> u8();
  [[nodiscard]] std::optional<bool> boolean();
  [[nodiscard]] std::optional<std::uint16_t> u16();
  [[nodiscard]] std::optional<std::uint32_t> u32();
  [[nodiscard]] std::optional<std::uint64_t> u64();
  [[nodiscard]] std::optional<std::int64_t> i64();
  [[nodiscard]] std::optional<double> f64();
  [[nodiscard]] std::optional<std::string> bytes();
  [[nodiscard]] std::optional<std::string> text();

  [[nodiscard]] bool ok() const noexcept { return status_ == ProtocolStatus::Ok; }
  [[nodiscard]] ProtocolStatus status() const noexcept { return status_; }
  /// Marks the reader as failed. Used by decoding helpers that detect a
  /// semantic error after bytes have already been consumed.
  void mark_invalid(ProtocolStatus status = ProtocolStatus::InvalidEnum) noexcept { fail(status); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  /// True when every byte has been consumed.
  [[nodiscard]] bool exhausted() const noexcept { return position_ == data_.size(); }

 private:
  [[nodiscard]] bool need(std::size_t count) noexcept;
  void fail(ProtocolStatus status) noexcept {
    if (status_ == ProtocolStatus::Ok) {
      status_ = status;
    }
  }

  std::string_view data_;
  std::size_t position_ = 0;
  ProtocolStatus status_ = ProtocolStatus::Ok;
};

struct FrameHeader {
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  MessageType type = MessageType::Invalid;
  std::uint32_t flags = 0;
  std::uint64_t sequence = 0;
  std::uint32_t payload_length = 0;
  std::uint32_t checksum = 0;
};

inline constexpr std::size_t kFrameHeaderSize = 28;

struct FrameOutcome {
  ProtocolStatus status = ProtocolStatus::Ok;
  FrameHeader header;
  std::string payload;
  /// Bytes consumed from the input, valid only when status is Ok.
  std::size_t consumed = 0;
  StructuredError error;

  [[nodiscard]] bool ok() const noexcept { return status == ProtocolStatus::Ok; }
};

/// Encodes one complete frame. Returns an empty string when the payload
/// exceeds p max_payload.
[[nodiscard]] std::string encode_frame(MessageType type, std::uint64_t sequence,
                                       std::string_view payload, std::uint32_t flags = 0,
                                       std::uint32_t max_payload = kMaxFramePayloadBytes);

/// Decodes one complete frame from the front of p bytes. p bytes may hold
/// more than one frame; p consumed reports how much was used.
[[nodiscard]] FrameOutcome decode_frame(std::string_view bytes,
                                        std::uint32_t max_payload = kMaxFramePayloadBytes);

/// CRC32 (IEEE, reflected, polynomial 0xEDB88320) over the given bytes.
[[nodiscard]] std::uint32_t crc32(std::string_view bytes) noexcept;

// ---------------------------------------------------------------------------
// Message payloads
// ---------------------------------------------------------------------------

struct HelloMessage {
  std::uint16_t protocol_version = kProtocolVersion;
  ClusterId cluster;
  RackAgentBootId boot;
  std::string role;
  std::string build;

  friend bool operator==(const HelloMessage&, const HelloMessage&) = default;
};

struct HelloAckMessage {
  std::uint16_t protocol_version = kProtocolVersion;
  ClusterId cluster;
  CoordinatorEpoch coordinator_epoch;
  ClusterEpoch cluster_epoch;
  TopologyEpoch topology_epoch;
  TopologyGeneration topology_generation;
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;
  bool accepted = false;
  RejectionReason reason = RejectionReason::None;
  std::string detail;

  friend bool operator==(const HelloAckMessage&, const HelloAckMessage&) = default;
};

struct RegisterPublisherMessage {
  ClusterId cluster;
  MutationAuthority authority;
  RackPublisherId publisher;
  RackId rack;
  RackGeneration generation;
  RackLifecycleState rack_lifecycle = RackLifecycleState::Unknown;
  RackCompositionSummary composition;
  std::string origin_label;
  Timestamp sent_at = Timestamp::unknown();

  friend bool operator==(const RegisterPublisherMessage&, const RegisterPublisherMessage&) = default;
};

struct RegisterAckMessage {
  bool accepted = false;
  RejectionReason reason = RejectionReason::None;
  CoordinatorEpoch coordinator_epoch;
  ClusterEpoch cluster_epoch;
  TopologyEpoch topology_epoch;
  TopologyGeneration topology_generation;
  MembershipGeneration membership_generation;
  std::string detail;

  friend bool operator==(const RegisterAckMessage&, const RegisterAckMessage&) = default;
};

struct PublishRequestMessage {
  MutationRequest request;

  friend bool operator==(const PublishRequestMessage&, const PublishRequestMessage&) = default;
};

struct PublishResultMessage {
  MutationResult result;

  friend bool operator==(const PublishResultMessage&, const PublishResultMessage&) = default;
};

struct HeartbeatMessage {
  ClusterId cluster;
  MutationAuthority authority;
  Timestamp sent_at = Timestamp::unknown();

  friend bool operator==(const HeartbeatMessage&, const HeartbeatMessage&) = default;
};

struct HeartbeatAckMessage {
  CoordinatorEpoch coordinator_epoch;
  ClusterEpoch cluster_epoch;
  TopologyEpoch topology_epoch;
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;
  bool accepted = false;
  RejectionReason reason = RejectionReason::None;
  std::string detail;

  friend bool operator==(const HeartbeatAckMessage&, const HeartbeatAckMessage&) = default;
};

/// Compact, bounded view of a snapshot. The full canonical state is never sent
/// over the wire.
struct SnapshotView {
  ClusterId cluster;
  ClusterEpoch cluster_epoch;
  CoordinatorEpoch coordinator_epoch;
  ClusterGeneration cluster_generation;
  MembershipGeneration membership_generation;
  TopologyEpoch topology_epoch;
  TopologyGeneration topology_generation;
  ConnectivityGeneration connectivity_generation;
  HealthGeneration health_generation;
  ConstraintGeneration constraint_generation;
  DomainGenerations domain_generations;
  SnapshotGeneration snapshot_generation;
  PublicationGeneration publication_generation;
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;
  std::vector<RackGenerationBinding> rack_bindings;
  std::string semantic_digest;
  std::size_t rack_count = 0;
  std::size_t link_count = 0;
  std::size_t domain_count = 0;

  [[nodiscard]] static SnapshotView from_snapshot(const ClusterSnapshot& snapshot);
  friend bool operator==(const SnapshotView&, const SnapshotView&) = default;
};

struct SnapshotRequestMessage {
  ClusterId cluster;

  friend bool operator==(const SnapshotRequestMessage&, const SnapshotRequestMessage&) = default;
};

struct SnapshotResponseMessage {
  bool accepted = false;
  RejectionReason reason = RejectionReason::None;
  std::string detail;
  SnapshotView view;

  friend bool operator==(const SnapshotResponseMessage&, const SnapshotResponseMessage&) = default;
};

struct ValidateSnapshotRequestMessage {
  SnapshotView view;

  friend bool operator==(const ValidateSnapshotRequestMessage&,
                         const ValidateSnapshotRequestMessage&) = default;
};

struct ValidateSnapshotResponseMessage {
  SnapshotValidation validation;

  friend bool operator==(const ValidateSnapshotResponseMessage&,
                         const ValidateSnapshotResponseMessage&) = default;
};

struct RevalidateRequestMessage {
  ClusterId cluster;
  MutationAuthority authority;
  /// Racks whose evidence the caller wants revalidated.
  std::vector<RackId> racks;

  friend bool operator==(const RevalidateRequestMessage&, const RevalidateRequestMessage&) = default;
};

struct RevalidateResponseMessage {
  bool accepted = false;
  RejectionReason reason = RejectionReason::None;
  std::string detail;
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;
  ReadinessEvaluation readiness;

  friend bool operator==(const RevalidateResponseMessage&, const RevalidateResponseMessage&) = default;
};

struct ShutdownMessage {
  std::string reason;

  friend bool operator==(const ShutdownMessage&, const ShutdownMessage&) = default;
};

struct ErrorMessage {
  StructuredError error;

  friend bool operator==(const ErrorMessage&, const ErrorMessage&) = default;
};

// Encoders. Each returns an empty string on a bounded-encoding failure.
[[nodiscard]] std::string encode_hello(const HelloMessage& message);
[[nodiscard]] std::string encode_hello_ack(const HelloAckMessage& message);
[[nodiscard]] std::string encode_register_publisher(const RegisterPublisherMessage& message);
[[nodiscard]] std::string encode_register_ack(const RegisterAckMessage& message);
[[nodiscard]] std::string encode_publish_request(const PublishRequestMessage& message);
[[nodiscard]] std::string encode_publish_result(const PublishResultMessage& message);
[[nodiscard]] std::string encode_heartbeat(const HeartbeatMessage& message);
[[nodiscard]] std::string encode_heartbeat_ack(const HeartbeatAckMessage& message);
[[nodiscard]] std::string encode_snapshot_request(const SnapshotRequestMessage& message);
[[nodiscard]] std::string encode_snapshot_response(const SnapshotResponseMessage& message);
[[nodiscard]] std::string encode_validate_request(const ValidateSnapshotRequestMessage& message);
[[nodiscard]] std::string encode_validate_response(const ValidateSnapshotResponseMessage& message);
[[nodiscard]] std::string encode_revalidate_request(const RevalidateRequestMessage& message);
[[nodiscard]] std::string encode_revalidate_response(const RevalidateResponseMessage& message);
[[nodiscard]] std::string encode_shutdown(const ShutdownMessage& message);
[[nodiscard]] std::string encode_error(const ErrorMessage& message);

// Decoders. A decode that does not consume the whole payload fails.
struct DecodeOutcome {
  ProtocolStatus status = ProtocolStatus::Ok;
  StructuredError error;

  [[nodiscard]] bool ok() const noexcept { return status == ProtocolStatus::Ok; }
};

[[nodiscard]] DecodeOutcome decode_hello(std::string_view payload, HelloMessage& out);
[[nodiscard]] DecodeOutcome decode_hello_ack(std::string_view payload, HelloAckMessage& out);
[[nodiscard]] DecodeOutcome decode_register_publisher(std::string_view payload,
                                                     RegisterPublisherMessage& out);
[[nodiscard]] DecodeOutcome decode_register_ack(std::string_view payload, RegisterAckMessage& out);
[[nodiscard]] DecodeOutcome decode_publish_request(std::string_view payload,
                                                   PublishRequestMessage& out);
[[nodiscard]] DecodeOutcome decode_publish_result(std::string_view payload,
                                                  PublishResultMessage& out);
[[nodiscard]] DecodeOutcome decode_heartbeat(std::string_view payload, HeartbeatMessage& out);
[[nodiscard]] DecodeOutcome decode_heartbeat_ack(std::string_view payload, HeartbeatAckMessage& out);
[[nodiscard]] DecodeOutcome decode_snapshot_request(std::string_view payload,
                                                    SnapshotRequestMessage& out);
[[nodiscard]] DecodeOutcome decode_snapshot_response(std::string_view payload,
                                                     SnapshotResponseMessage& out);
[[nodiscard]] DecodeOutcome decode_validate_request(std::string_view payload,
                                                    ValidateSnapshotRequestMessage& out);
[[nodiscard]] DecodeOutcome decode_validate_response(std::string_view payload,
                                                     ValidateSnapshotResponseMessage& out);
[[nodiscard]] DecodeOutcome decode_revalidate_request(std::string_view payload,
                                                      RevalidateRequestMessage& out);
[[nodiscard]] DecodeOutcome decode_revalidate_response(std::string_view payload,
                                                       RevalidateResponseMessage& out);
[[nodiscard]] DecodeOutcome decode_shutdown(std::string_view payload, ShutdownMessage& out);
[[nodiscard]] DecodeOutcome decode_error(std::string_view payload, ErrorMessage& out);

// Mutation payload codecs, also used by persistence for durable fields.
[[nodiscard]] bool encode_mutation_request(const MutationRequest& request, ByteWriter& writer);
[[nodiscard]] DecodeOutcome decode_mutation_request(ByteReader& reader, MutationRequest& out);
[[nodiscard]] bool encode_mutation_result(const MutationResult& result, ByteWriter& writer);
[[nodiscard]] DecodeOutcome decode_mutation_result(ByteReader& reader, MutationResult& out);
[[nodiscard]] bool encode_snapshot_view(const SnapshotView& view, ByteWriter& writer);
[[nodiscard]] DecodeOutcome decode_snapshot_view(ByteReader& reader, SnapshotView& out);
[[nodiscard]] bool encode_structured_error(const StructuredError& error, ByteWriter& writer);
[[nodiscard]] DecodeOutcome decode_structured_error(ByteReader& reader, StructuredError& out);

}  // namespace cluster_fabric

#endif  // CLUSTER_FABRIC_PROTOCOL_HPP
