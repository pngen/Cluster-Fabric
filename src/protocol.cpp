// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Bounded framed-protocol codecs.
//
// This translation unit implements the little-endian byte reader and writer,
// the CRC32 framing, and a deterministic version-tagged payload codec for every
// message declared in protocol.hpp. Every length and count is validated against
// the hard bounds in limits.hpp before a byte is read or an allocation is
// sized, the first failure wins, decoding never accepts an out-of-range
// enumerator or an unparsable identity, and nothing in this file throws.

#include "cluster_fabric/protocol.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cluster_fabric {

// ---------------------------------------------------------------------------
// Status and message-type names
// ---------------------------------------------------------------------------

std::string_view to_string(ProtocolStatus value) noexcept {
  switch (value) {
    case ProtocolStatus::Ok: return "OK";
    case ProtocolStatus::BadMagic: return "BAD_MAGIC";
    case ProtocolStatus::UnsupportedVersion: return "UNSUPPORTED_VERSION";
    case ProtocolStatus::TruncatedHeader: return "TRUNCATED_HEADER";
    case ProtocolStatus::TruncatedPayload: return "TRUNCATED_PAYLOAD";
    case ProtocolStatus::OversizedFrame: return "OVERSIZED_FRAME";
    case ProtocolStatus::ChecksumMismatch: return "CHECKSUM_MISMATCH";
    case ProtocolStatus::UnknownMessage: return "UNKNOWN_MESSAGE";
    case ProtocolStatus::MalformedPayload: return "MALFORMED_PAYLOAD";
    case ProtocolStatus::AbsurdLength: return "ABSURD_LENGTH";
    case ProtocolStatus::TrailingGarbage: return "TRAILING_GARBAGE";
    case ProtocolStatus::InvalidEnum: return "INVALID_ENUM";
    case ProtocolStatus::InvalidIdentity: return "INVALID_IDENTITY";
    case ProtocolStatus::ConnectionClosed: return "CONNECTION_CLOSED";
    case ProtocolStatus::IoError: return "IO_ERROR";
    case ProtocolStatus::NotConnected: return "NOT_CONNECTED";
    case ProtocolStatus::AlreadyRunning: return "ALREADY_RUNNING";
    case ProtocolStatus::NotRunning: return "NOT_RUNNING";
    case ProtocolStatus::ShutdownInProgress: return "SHUTDOWN_IN_PROGRESS";
    case ProtocolStatus::Rejected: return "REJECTED";
  }
  return "UNKNOWN";
}

namespace {

struct MessageTypeName {
  MessageType type;
  std::string_view name;
};

/// Declared enumerators of MessageType, in declaration order. The same table
/// renders and parses, so the two directions cannot drift apart.
constexpr MessageTypeName kMessageTypeNames[] = {
    {MessageType::Invalid, "INVALID"},
    {MessageType::Hello, "HELLO"},
    {MessageType::HelloAck, "HELLO_ACK"},
    {MessageType::RegisterPublisher, "REGISTER_PUBLISHER"},
    {MessageType::RegisterAck, "REGISTER_ACK"},
    {MessageType::PublishRequest, "PUBLISH_REQUEST"},
    {MessageType::PublishResult, "PUBLISH_RESULT"},
    {MessageType::Heartbeat, "HEARTBEAT"},
    {MessageType::HeartbeatAck, "HEARTBEAT_ACK"},
    {MessageType::SnapshotRequest, "SNAPSHOT_REQUEST"},
    {MessageType::SnapshotResponse, "SNAPSHOT_RESPONSE"},
    {MessageType::ValidateSnapshotRequest, "VALIDATE_SNAPSHOT_REQUEST"},
    {MessageType::ValidateSnapshotResponse, "VALIDATE_SNAPSHOT_RESPONSE"},
    {MessageType::RevalidateRequest, "REVALIDATE_REQUEST"},
    {MessageType::RevalidateResponse, "REVALIDATE_RESPONSE"},
    {MessageType::Shutdown, "SHUTDOWN"},
    {MessageType::Error, "ERROR"},
};

}  // namespace

std::string_view to_string(MessageType value) noexcept {
  for (const MessageTypeName& entry : kMessageTypeNames) {
    if (entry.type == value) {
      return entry.name;
    }
  }
  return "UNKNOWN";
}

std::optional<MessageType> message_type_from_string(std::string_view text) noexcept {
  for (const MessageTypeName& entry : kMessageTypeNames) {
    if (entry.name == text) {
      return entry.type;
    }
  }
  return std::nullopt;
}

namespace {

// ---------------------------------------------------------------------------
// Bounds
// ---------------------------------------------------------------------------

/// Maximum total bytes one ByteWriter produces. Bounded by the persistence
/// container ceiling because the same writer also encodes durable records.
constexpr std::size_t kMaxWriterBytes = static_cast<std::size_t>(kMaxPersistenceBytes);

/// Maximum bytes a ByteWriter reserves up front from a caller hint.
constexpr std::size_t kMaxWriterReserve = static_cast<std::size_t>(kMaxFramePayloadBytes);

/// Declared maxima of every enumerator that crosses the wire. A raw value
/// above its maximum is never accepted.
constexpr std::uint8_t kMaxMessageTypeRaw = 16;
constexpr std::uint8_t kMaxEvidenceProvenanceRaw = 6;
constexpr std::uint8_t kMaxFreshnessRaw = 3;
constexpr std::uint8_t kMaxClusterLifecycleRaw = 7;
constexpr std::uint8_t kMaxRackMembershipStateRaw = 4;
constexpr std::uint8_t kMaxRackLifecycleStateRaw = 6;
constexpr std::uint8_t kMaxRackCurrentnessRaw = 3;
constexpr std::uint8_t kMaxFailureDomainClassRaw = 9;
constexpr std::uint8_t kMaxPlacementDomainClassRaw = 7;
constexpr std::uint8_t kMaxCapacityDomainClassRaw = 5;
constexpr std::uint8_t kMaxConnectivityClassRaw = 5;
constexpr std::uint8_t kMaxLinkDirectionRaw = 2;
constexpr std::uint8_t kMaxReachabilityRaw = 3;
constexpr std::uint8_t kMaxBandwidthClassRaw = 4;
constexpr std::uint8_t kMaxLatencyClassRaw = 4;
constexpr std::uint8_t kMaxAcceleratorVendorRaw = 4;
constexpr std::uint8_t kMaxHealthStateRaw = 3;
constexpr std::uint8_t kMaxConstraintKindRaw = 5;
constexpr std::uint8_t kMaxErrorCategoryRaw = 10;
constexpr std::uint8_t kMaxErrorStageRaw = 13;
constexpr std::uint8_t kMaxMutationKindRaw = 25;
constexpr std::uint8_t kMaxMutationOutcomeRaw = 3;
constexpr std::uint8_t kMaxRejectionReasonRaw = 21;
constexpr std::uint8_t kMaxSnapshotStaleReasonRaw = 24;

/// Maximum accelerator classes in one composition summary, matching the bound
/// enforced when a rack reference is validated.
constexpr std::size_t kMaxAcceleratorClasses = kMaxCapacityQuantities;
/// Maximum required placement-domain classes in a readiness contract.
constexpr std::size_t kMaxRequiredPlacementClasses =
    static_cast<std::size_t>(kMaxPlacementDomainClassRaw) + 1u;
/// Maximum required failure-domain classes in a readiness contract.
constexpr std::size_t kMaxRequiredFailureClasses =
    static_cast<std::size_t>(kMaxFailureDomainClassRaw) + 1u;
/// Maximum stale reasons in one snapshot validation.
constexpr std::size_t kMaxStaleReasons =
    static_cast<std::size_t>(kMaxSnapshotStaleReasonRaw) + 1u;

// ---------------------------------------------------------------------------
// Shared failure helpers
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256u; ++index) {
    std::uint32_t crc = index;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    table[index] = crc;
  }
  return table;
}

/// Built once at compile time; the reflected IEEE polynomial is applied
/// table-driven, one byte at a time.
constexpr std::array<std::uint32_t, 256> kCrc32Table = make_crc32_table();

/// One step of the reflected CRC32. Callers keep the pre/post inversion
/// themselves so a frame checksum can span the two non-contiguous regions of a
/// frame: the 24 header bytes that precede the checksum field, then the
/// payload.
[[nodiscard]] constexpr std::uint32_t crc32_extend(std::uint32_t crc,
                                                  std::string_view bytes) noexcept {
  for (const char c : bytes) {
    const std::uint32_t index =
        (crc ^ static_cast<std::uint32_t>(static_cast<unsigned char>(c))) & 0xFFu;
    crc = (crc >> 8) ^ kCrc32Table[index];
  }
  return crc;
}

/// Stable lowercase reason code derived from the status name, used for
/// StructuredError::reason.
[[nodiscard]] std::string reason_code(ProtocolStatus status) {
  std::string text(to_string(status));
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

[[nodiscard]] DecodeOutcome decode_error(ProtocolStatus status, std::string subject,
                                         std::string message) {
  DecodeOutcome outcome;
  outcome.status = status;
  outcome.error = StructuredError::make(ErrorCategory::Protocol, ErrorStage::Decode,
                                        reason_code(status), std::move(message));
  outcome.error.subject = std::move(subject);
  return outcome;
}

/// Fails an outcome that already carries whatever header fields were parsed,
/// so a caller can act on a declared payload length even when the payload has
/// not arrived yet.
[[nodiscard]] FrameOutcome frame_failure(FrameOutcome outcome, ProtocolStatus status,
                                         std::string_view subject, std::string message) {
  outcome.status = status;
  outcome.error = StructuredError::make(ErrorCategory::Protocol, ErrorStage::Decode,
                                        reason_code(status), std::move(message));
  outcome.error.subject = std::string(subject);
  outcome.payload.clear();
  outcome.consumed = 0;
  return outcome;
}

[[nodiscard]] FrameOutcome frame_error(ProtocolStatus status, std::string_view subject,
                                       std::string message) {
  return frame_failure(FrameOutcome{}, status, subject, std::move(message));
}

[[nodiscard]] bool u64_fits_size_t(std::uint64_t value) noexcept {
  constexpr std::uint64_t kSizeTMax =
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
  return value <= kSizeTMax;
}

}  // namespace

// ---------------------------------------------------------------------------
// ByteWriter
// ---------------------------------------------------------------------------

ByteWriter::ByteWriter(std::size_t reserve) {
  // The requested capacity is a hint only; it is clamped so an absurd hint can
  // never force an unbounded allocation.
  data_.reserve(reserve > kMaxWriterReserve ? kMaxWriterReserve : reserve);
}

void ByteWriter::u8(std::uint8_t value) {
  if (!ok()) {
    return;
  }
  if (data_.size() + 1u > kMaxWriterBytes) {
    fail(ProtocolStatus::AbsurdLength);
    return;
  }
  data_.push_back(static_cast<char>(value));
}

void ByteWriter::boolean(bool value) { u8(value ? std::uint8_t{1} : std::uint8_t{0}); }

void ByteWriter::u16(std::uint16_t value) {
  for (int index = 0; index < 2; ++index) {
    u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
}

void ByteWriter::u32(std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
}

void ByteWriter::u64(std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
  }
}

void ByteWriter::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void ByteWriter::f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }

void ByteWriter::bytes(std::string_view value) {
  if (!ok()) {
    return;
  }
  if (value.size() > kMaxEncodedStringBytes) {
    fail(ProtocolStatus::AbsurdLength);
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  if (!ok() || value.empty()) {
    return;
  }
  if (data_.size() + value.size() > kMaxWriterBytes) {
    fail(ProtocolStatus::AbsurdLength);
    return;
  }
  data_.append(value.data(), value.size());
}

void ByteWriter::text(std::string_view value) { bytes(value); }

void ByteWriter::optional_present(bool present) { u8(present ? std::uint8_t{1} : std::uint8_t{0}); }

// ---------------------------------------------------------------------------
// ByteReader
// ---------------------------------------------------------------------------

bool ByteReader::need(std::size_t count) noexcept {
  if (status_ != ProtocolStatus::Ok) {
    return false;
  }
  if (count > remaining()) {
    fail(ProtocolStatus::TruncatedPayload);
    return false;
  }
  return true;
}

std::optional<std::uint8_t> ByteReader::u8() {
  if (!need(1)) {
    return std::nullopt;
  }
  const std::uint8_t value = static_cast<std::uint8_t>(static_cast<unsigned char>(data_[position_]));
  ++position_;
  return value;
}

std::optional<bool> ByteReader::boolean() {
  const std::optional<std::uint8_t> value = u8();
  if (!value.has_value()) {
    return std::nullopt;
  }
  if (*value > 1) {
    fail(ProtocolStatus::MalformedPayload);
    return std::nullopt;
  }
  return *value == 1;
}

std::optional<std::uint16_t> ByteReader::u16() {
  std::uint16_t value = 0;
  for (int index = 0; index < 2; ++index) {
    const std::optional<std::uint8_t> byte = u8();
    if (!byte.has_value()) {
      return std::nullopt;
    }
    value = static_cast<std::uint16_t>(
        value | static_cast<std::uint16_t>(static_cast<std::uint16_t>(*byte) << (index * 8)));
  }
  return value;
}

std::optional<std::uint32_t> ByteReader::u32() {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    const std::optional<std::uint8_t> byte = u8();
    if (!byte.has_value()) {
      return std::nullopt;
    }
    value |= static_cast<std::uint32_t>(*byte) << (index * 8);
  }
  return value;
}

std::optional<std::uint64_t> ByteReader::u64() {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    const std::optional<std::uint8_t> byte = u8();
    if (!byte.has_value()) {
      return std::nullopt;
    }
    value |= static_cast<std::uint64_t>(*byte) << (index * 8);
  }
  return value;
}

std::optional<std::int64_t> ByteReader::i64() {
  const std::optional<std::uint64_t> value = u64();
  if (!value.has_value()) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(*value);
}

std::optional<double> ByteReader::f64() {
  const std::optional<std::uint64_t> value = u64();
  if (!value.has_value()) {
    return std::nullopt;
  }
  return std::bit_cast<double>(*value);
}

std::optional<std::string> ByteReader::bytes() {
  const std::optional<std::uint32_t> length = u32();
  if (!length.has_value()) {
    return std::nullopt;
  }
  const std::size_t size = static_cast<std::size_t>(*length);
  if (size > kMaxEncodedStringBytes) {
    fail(ProtocolStatus::AbsurdLength);
    return std::nullopt;
  }
  if (size > remaining()) {
    fail(ProtocolStatus::TruncatedPayload);
    return std::nullopt;
  }
  std::string value(data_.substr(position_, size));
  position_ += size;
  return value;
}

std::optional<std::string> ByteReader::text() { return bytes(); }

// ---------------------------------------------------------------------------
// CRC32 and framing
// ---------------------------------------------------------------------------

std::uint32_t crc32(std::string_view bytes) noexcept {
  return crc32_extend(0xFFFFFFFFu, bytes) ^ 0xFFFFFFFFu;
}

std::string encode_frame(MessageType type, std::uint64_t sequence, std::string_view payload,
                         std::uint32_t flags, std::uint32_t max_payload) {
  const std::uint32_t bounded_max =
      max_payload > kAbsoluteMaxFramePayloadBytes ? kAbsoluteMaxFramePayloadBytes : max_payload;
  if (payload.size() > bounded_max) {
    return std::string();
  }
  ByteWriter header(0);
  header.u32(kProtocolMagic);
  header.u16(kProtocolVersion);
  header.u16(static_cast<std::uint16_t>(type));
  header.u32(flags);
  header.u64(sequence);
  header.u32(static_cast<std::uint32_t>(payload.size()));
  if (!header.ok() || header.data().size() != kFrameHeaderSize - 4u) {
    return std::string();
  }
  std::string frame(header.data());
  frame.append(payload);
  // The checksum covers the 24 header bytes that precede it plus the payload.
  const std::uint32_t checksum = crc32(frame);
  char raw[4] = {};
  for (int index = 0; index < 4; ++index) {
    raw[index] = static_cast<char>((checksum >> (index * 8)) & 0xFFu);
  }
  frame.insert(kFrameHeaderSize - 4u, raw, 4u);
  return frame;
}

FrameOutcome decode_frame(std::string_view bytes, std::uint32_t max_payload) {
  const std::uint32_t bounded_max =
      max_payload > kAbsoluteMaxFramePayloadBytes ? kAbsoluteMaxFramePayloadBytes : max_payload;
  if (bytes.size() < kFrameHeaderSize) {
    return frame_error(ProtocolStatus::TruncatedHeader, "frame",
                       "fewer than 28 bytes are available for a frame header");
  }
  ByteReader reader(bytes);
  const std::optional<std::uint32_t> magic = reader.u32();
  const std::optional<std::uint16_t> version = reader.u16();
  const std::optional<std::uint16_t> raw_type = reader.u16();
  const std::optional<std::uint32_t> flags = reader.u32();
  const std::optional<std::uint64_t> sequence = reader.u64();
  const std::optional<std::uint32_t> payload_length = reader.u32();
  const std::optional<std::uint32_t> checksum = reader.u32();
  if (!magic.has_value() || !version.has_value() || !raw_type.has_value() || !flags.has_value() ||
      !sequence.has_value() || !payload_length.has_value() || !checksum.has_value()) {
    return frame_error(ProtocolStatus::TruncatedHeader, "frame", "frame header is truncated");
  }
  // The parsed header is reported with every failure so a caller can read the
  // declared payload length from a frame whose body has not arrived yet.
  FrameOutcome outcome;
  outcome.header.magic = *magic;
  outcome.header.version = *version;
  outcome.header.type = static_cast<MessageType>(*raw_type);
  outcome.header.flags = *flags;
  outcome.header.sequence = *sequence;
  outcome.header.payload_length = *payload_length;
  outcome.header.checksum = *checksum;
  if (*magic != kProtocolMagic) {
    return frame_failure(outcome, ProtocolStatus::BadMagic, "frame",
                         "frame magic does not match CPF1");
  }
  if (*version != kProtocolVersion) {
    return frame_failure(outcome, ProtocolStatus::UnsupportedVersion, "frame",
                         "frame protocol version " + std::to_string(*version) + " is not supported");
  }
  if (static_cast<std::uint8_t>(*raw_type) > kMaxMessageTypeRaw) {
    return frame_failure(outcome, ProtocolStatus::UnknownMessage, "frame",
                         "frame message type " + std::to_string(*raw_type) + " is not declared");
  }
  if (*payload_length > bounded_max) {
    return frame_failure(outcome, ProtocolStatus::OversizedFrame, "frame",
                         "declared payload length " + std::to_string(*payload_length) +
                             " exceeds the configured maximum " + std::to_string(bounded_max));
  }
  if (static_cast<std::size_t>(*payload_length) > reader.remaining()) {
    return frame_failure(outcome, ProtocolStatus::TruncatedPayload, "frame",
                         "declared payload length " + std::to_string(*payload_length) +
                             " exceeds the available bytes");
  }
  // The checksum covers the 24 header bytes that precede it, then the payload.
  std::uint32_t computed = crc32_extend(0xFFFFFFFFu, bytes.substr(0, kFrameHeaderSize - 4u));
  computed = crc32_extend(computed, bytes.substr(kFrameHeaderSize, *payload_length));
  if ((computed ^ 0xFFFFFFFFu) != *checksum) {
    return frame_failure(outcome, ProtocolStatus::ChecksumMismatch, "frame",
                         "frame checksum does not match the header and payload");
  }
  outcome.payload = std::string(bytes.substr(kFrameHeaderSize, *payload_length));
  outcome.consumed = kFrameHeaderSize + static_cast<std::size_t>(*payload_length);
  return outcome;
}


namespace {

// ---------------------------------------------------------------------------
// Bounded wire helpers
// ---------------------------------------------------------------------------

/// Writer context. The ByteWriter records its own failures; this adds the one
/// bound it cannot express: a count above its declared maximum.
struct WireWriter {
  ByteWriter& writer;
  bool ok = true;

  [[nodiscard]] bool good() const noexcept { return ok && writer.ok(); }
  void reject() noexcept { ok = false; }
};

/// Reader context. As in ByteReader the first failure wins and every later
/// read is a no-op.
struct WireReader {
  ByteReader& reader;
  ProtocolStatus status = ProtocolStatus::Ok;
  std::string field;

  [[nodiscard]] bool ok() const noexcept { return status == ProtocolStatus::Ok; }
};

void fail_field(WireReader& w, ProtocolStatus status, const char* name) {
  if (w.status == ProtocolStatus::Ok) {
    w.status = status;
    w.field = name;
  }
  // Record the semantic failure on the reader as well, so a caller that only
  // inspects the ByteReader still sees why decoding stopped.
  w.reader.mark_invalid(status);
}

[[nodiscard]] bool read_u8(WireReader& w, std::uint8_t& out, const char* name) {
  const std::optional<std::uint8_t> value = w.reader.u8();
  if (!value.has_value()) {
    fail_field(w, w.reader.status(), name);
    return false;
  }
  out = *value;
  return true;
}

[[nodiscard]] bool read_u16(WireReader& w, std::uint16_t& out, const char* name) {
  const std::optional<std::uint16_t> value = w.reader.u16();
  if (!value.has_value()) {
    fail_field(w, w.reader.status(), name);
    return false;
  }
  out = *value;
  return true;
}

[[nodiscard]] bool read_u32(WireReader& w, std::uint32_t& out, const char* name) {
  const std::optional<std::uint32_t> value = w.reader.u32();
  if (!value.has_value()) {
    fail_field(w, w.reader.status(), name);
    return false;
  }
  out = *value;
  return true;
}

[[nodiscard]] bool read_u64(WireReader& w, std::uint64_t& out, const char* name) {
  const std::optional<std::uint64_t> value = w.reader.u64();
  if (!value.has_value()) {
    fail_field(w, w.reader.status(), name);
    return false;
  }
  out = *value;
  return true;
}

[[nodiscard]] bool read_i64(WireReader& w, std::int64_t& out, const char* name) {
  const std::optional<std::int64_t> value = w.reader.i64();
  if (!value.has_value()) {
    fail_field(w, w.reader.status(), name);
    return false;
  }
  out = *value;
  return true;
}

[[nodiscard]] bool read_f64(WireReader& w, double& out, const char* name) {
  const std::optional<double> value = w.reader.f64();
  if (!value.has_value()) {
    fail_field(w, w.reader.status(), name);
    return false;
  }
  out = *value;
  return true;
}

[[nodiscard]] bool read_bool(WireReader& w, bool& out, const char* name) {
  const std::optional<bool> value = w.reader.boolean();
  if (!value.has_value()) {
    fail_field(w, w.reader.status(), name);
    return false;
  }
  out = *value;
  return true;
}

[[nodiscard]] bool read_string(WireReader& w, std::string& out, const char* name) {
  const std::optional<std::string> value = w.reader.text();
  if (!value.has_value()) {
    fail_field(w, w.reader.status(), name);
    return false;
  }
  out = *value;
  return true;
}

[[nodiscard]] bool read_size(WireReader& w, std::size_t& out, const char* name) {
  std::uint64_t raw_value = 0;
  if (!read_u64(w, raw_value, name)) {
    return false;
  }
  if (!u64_fits_size_t(raw_value)) {
    fail_field(w, ProtocolStatus::AbsurdLength, name);
    return false;
  }
  out = static_cast<std::size_t>(raw_value);
  return true;
}

[[nodiscard]] bool read_present(WireReader& w, bool& out, const char* name) {
  std::uint8_t marker = 0;
  if (!read_u8(w, marker, name)) {
    return false;
  }
  if (marker > 1) {
    fail_field(w, ProtocolStatus::MalformedPayload, name);
    return false;
  }
  out = marker == 1;
  return true;
}

template <class Enum>
[[nodiscard]] bool read_enum(WireReader& w, Enum& out, std::uint8_t max_raw, const char* name) {
  std::uint8_t raw_value = 0;
  if (!read_u8(w, raw_value, name)) {
    return false;
  }
  if (raw_value > max_raw) {
    fail_field(w, ProtocolStatus::InvalidEnum, name);
    return false;
  }
  out = static_cast<Enum>(raw_value);
  return true;
}

/// Parses an identity through the type's own parse(), so the accepted alphabet
/// and length are exactly the ones the identity type defines.
template <class Id>
[[nodiscard]] std::optional<Id> read_identity_value(WireReader& w, const char* name) {
  std::string text;
  if (!read_string(w, text, name)) {
    return std::nullopt;
  }
  std::optional<Id> parsed = Id::parse(text);
  if (!parsed.has_value()) {
    fail_field(w, ProtocolStatus::InvalidIdentity, name);
    return std::nullopt;
  }
  return parsed;
}

template <class Id>
[[nodiscard]] bool read_optional_identity(WireReader& w, std::optional<Id>& out, const char* name) {
  bool present = false;
  if (!read_present(w, present, name)) {
    return false;
  }
  if (!present) {
    out.reset();
    return true;
  }
  std::optional<Id> parsed = read_identity_value<Id>(w, name);
  if (!parsed.has_value()) {
    return false;
  }
  out = std::move(parsed);
  return true;
}

template <class Counter>
[[nodiscard]] std::optional<Counter> read_counter_value(WireReader& w, const char* name) {
  std::uint64_t raw_value = 0;
  if (!read_u64(w, raw_value, name)) {
    return std::nullopt;
  }
  return Counter::from_raw(raw_value);
}

template <class Counter>
[[nodiscard]] bool read_counter(WireReader& w, Counter& out, const char* name) {
  std::uint64_t raw_value = 0;
  if (!read_u64(w, raw_value, name)) {
    return false;
  }
  out = Counter::from_raw(raw_value);
  return true;
}

template <class Counter>
[[nodiscard]] bool read_optional_counter(WireReader& w, std::optional<Counter>& out,
                                         const char* name) {
  bool present = false;
  if (!read_present(w, present, name)) {
    return false;
  }
  if (!present) {
    out.reset();
    return true;
  }
  std::optional<Counter> parsed = read_counter_value<Counter>(w, name);
  if (!parsed.has_value()) {
    return false;
  }
  out = *parsed;
  return true;
}

[[nodiscard]] bool read_optional_u32(WireReader& w, std::optional<std::uint32_t>& out,
                                     const char* name) {
  bool present = false;
  if (!read_present(w, present, name)) {
    return false;
  }
  if (!present) {
    out.reset();
    return true;
  }
  std::uint32_t value = 0;
  if (!read_u32(w, value, name)) {
    return false;
  }
  out = value;
  return true;
}

[[nodiscard]] bool read_optional_u64(WireReader& w, std::optional<std::uint64_t>& out,
                                     const char* name) {
  bool present = false;
  if (!read_present(w, present, name)) {
    return false;
  }
  if (!present) {
    out.reset();
    return true;
  }
  std::uint64_t value = 0;
  if (!read_u64(w, value, name)) {
    return false;
  }
  out = value;
  return true;
}

[[nodiscard]] bool read_optional_f64(WireReader& w, std::optional<double>& out, const char* name) {
  bool present = false;
  if (!read_present(w, present, name)) {
    return false;
  }
  if (!present) {
    out.reset();
    return true;
  }
  double value = 0.0;
  if (!read_f64(w, value, name)) {
    return false;
  }
  out = value;
  return true;
}

/// Reads a u32 count, rejecting a count above its declared bound and a count
/// that cannot fit in the bytes that remain. Every entry needs at least one
/// byte, so the second check makes every later reserve() safe.
[[nodiscard]] bool read_count(WireReader& w, std::size_t bound, std::size_t& out,
                              const char* name) {
  std::uint32_t raw_count = 0;
  if (!read_u32(w, raw_count, name)) {
    return false;
  }
  const std::size_t count = static_cast<std::size_t>(raw_count);
  if (count > bound) {
    fail_field(w, ProtocolStatus::AbsurdLength, name);
    return false;
  }
  if (count > w.reader.remaining()) {
    fail_field(w, ProtocolStatus::AbsurdLength, name);
    return false;
  }
  out = count;
  return true;
}

template <class T>
[[nodiscard]] bool read_vector(WireReader& w, std::vector<T>& out, std::size_t bound,
                               const char* name,
                               std::optional<T> (*read_one)(WireReader&, const char*)) {
  std::size_t count = 0;
  if (!read_count(w, bound, count, name)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    std::optional<T> entry = read_one(w, name);
    if (!entry.has_value()) {
      return false;
    }
    out.push_back(std::move(*entry));
  }
  return true;
}

template <class Id>
[[nodiscard]] bool read_identity_vector(WireReader& w, std::vector<Id>& out, std::size_t bound,
                                        const char* name) {
  std::size_t count = 0;
  if (!read_count(w, bound, count, name)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    std::optional<Id> entry = read_identity_value<Id>(w, name);
    if (!entry.has_value()) {
      return false;
    }
    out.push_back(std::move(*entry));
  }
  return true;
}

template <class Enum>
[[nodiscard]] bool read_enum_vector(WireReader& w, std::vector<Enum>& out, std::uint8_t max_raw,
                                    std::size_t bound, const char* name) {
  std::size_t count = 0;
  if (!read_count(w, bound, count, name)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    Enum entry{};
    if (!read_enum(w, entry, max_raw, name)) {
      return false;
    }
    out.push_back(entry);
  }
  return true;
}

[[nodiscard]] bool read_string_vector(WireReader& w, std::vector<std::string>& out,
                                      std::size_t bound, const char* name) {
  std::size_t count = 0;
  if (!read_count(w, bound, count, name)) {
    return false;
  }
  out.clear();
  out.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    std::string entry;
    if (!read_string(w, entry, name)) {
      return false;
    }
    out.push_back(std::move(entry));
  }
  return true;
}

template <class T>
[[nodiscard]] bool read_optional_record(WireReader& w, std::optional<T>& out, const char* name,
                                        std::optional<T> (*read_one)(WireReader&, const char*)) {
  bool present = false;
  if (!read_present(w, present, name)) {
    return false;
  }
  if (!present) {
    out.reset();
    return true;
  }
  std::optional<T> value = read_one(w, name);
  if (!value.has_value()) {
    return false;
  }
  out = std::move(value);
  return true;
}

// ---------------------------------------------------------------------------
// Writer primitives
// ---------------------------------------------------------------------------

void write_version_tag(WireWriter& w) { w.writer.u16(kProtocolVersion); }

[[nodiscard]] bool read_version_tag(WireReader& w, const char* name) {
  std::uint16_t version = 0;
  if (!read_u16(w, version, name)) {
    return false;
  }
  if (version != kProtocolVersion) {
    fail_field(w, ProtocolStatus::UnsupportedVersion, name);
    return false;
  }
  return true;
}

template <class Enum>
void write_enum(WireWriter& w, Enum value) {
  w.writer.u8(static_cast<std::uint8_t>(value));
}

template <class Id>
void write_identity(WireWriter& w, const Id& value) {
  w.writer.text(value.view());
}

template <class Id>
void write_optional_identity(WireWriter& w, const std::optional<Id>& value) {
  w.writer.optional_present(value.has_value());
  if (value.has_value()) {
    w.writer.text(value->view());
  }
}

template <class Counter>
void write_counter(WireWriter& w, Counter value) {
  w.writer.u64(value.value());
}

template <class Counter>
void write_optional_counter(WireWriter& w, const std::optional<Counter>& value) {
  w.writer.optional_present(value.has_value());
  if (value.has_value()) {
    w.writer.u64(value->value());
  }
}

void write_optional_u32(WireWriter& w, const std::optional<std::uint32_t>& value) {
  w.writer.optional_present(value.has_value());
  if (value.has_value()) {
    w.writer.u32(*value);
  }
}

void write_optional_u64(WireWriter& w, const std::optional<std::uint64_t>& value) {
  w.writer.optional_present(value.has_value());
  if (value.has_value()) {
    w.writer.u64(*value);
  }
}

void write_optional_f64(WireWriter& w, const std::optional<double>& value) {
  w.writer.optional_present(value.has_value());
  if (value.has_value()) {
    w.writer.f64(*value);
  }
}

void write_timestamp(WireWriter& w, Timestamp value) { w.writer.i64(value.millis()); }

void write_count(WireWriter& w, std::size_t count, std::size_t bound) {
  if (!w.good()) {
    return;
  }
  if (count > bound) {
    w.reject();
    return;
  }
  w.writer.u32(static_cast<std::uint32_t>(count));
}

template <class T, class Write>
void write_vector(WireWriter& w, const std::vector<T>& values, std::size_t bound,
                  Write&& write_one) {
  write_count(w, values.size(), bound);
  if (!w.good()) {
    return;
  }
  for (const T& value : values) {
    write_one(value);
    if (!w.good()) {
      return;
    }
  }
}

template <class Id>
void write_identity_vector(WireWriter& w, const std::vector<Id>& values, std::size_t bound) {
  write_count(w, values.size(), bound);
  if (!w.good()) {
    return;
  }
  for (const Id& value : values) {
    w.writer.text(value.view());
  }
}

template <class Enum>
void write_enum_vector(WireWriter& w, const std::vector<Enum>& values, std::size_t bound) {
  write_count(w, values.size(), bound);
  if (!w.good()) {
    return;
  }
  for (Enum value : values) {
    w.writer.u8(static_cast<std::uint8_t>(value));
  }
}

template <class T, class Write>
void write_optional_record(WireWriter& w, const std::optional<T>& value, Write&& write_one) {
  w.writer.optional_present(value.has_value());
  if (value.has_value()) {
    write_one(w, *value);
  }
}

[[nodiscard]] std::string finish_payload(ByteWriter& writer, const WireWriter& w) {
  if (!w.good()) {
    return std::string();
  }
  return std::move(writer).take();
}

[[nodiscard]] DecodeOutcome make_decode_error(const WireReader& w, std::string_view subject) {
  ProtocolStatus status = w.status;
  if (status == ProtocolStatus::Ok) {
    status = w.reader.status();
  }
  std::string message = "field ";
  message.append(w.field.empty() ? std::string_view("<payload>") : std::string_view(w.field));
  message.append(" was rejected");
  return decode_error(status, std::string(subject), std::move(message));
}

[[nodiscard]] DecodeOutcome trailing_garbage(std::size_t remaining, std::string_view subject) {
  std::string message = std::to_string(remaining);
  message.append(" byte(s) remain after the message payload");
  return decode_error(ProtocolStatus::TrailingGarbage, std::string(subject), std::move(message));
}


// ---------------------------------------------------------------------------
// Evidence, errors and explanations
// ---------------------------------------------------------------------------

void write_evidence_stamp(WireWriter& w, const EvidenceStamp& stamp) {
  write_enum(w, stamp.provenance);
  w.writer.i64(stamp.observed_at.millis());
  write_enum(w, stamp.freshness);
  w.writer.i64(stamp.ttl_millis);
}

[[nodiscard]] std::optional<EvidenceStamp> read_evidence_stamp(WireReader& w, const char* name) {
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  if (!read_enum(w, provenance, kMaxEvidenceProvenanceRaw, name)) {
    return std::nullopt;
  }
  std::int64_t observed_at = 0;
  if (!read_i64(w, observed_at, name)) {
    return std::nullopt;
  }
  Freshness freshness = Freshness::Unknown;
  if (!read_enum(w, freshness, kMaxFreshnessRaw, name)) {
    return std::nullopt;
  }
  std::int64_t ttl_millis = 0;
  if (!read_i64(w, ttl_millis, name)) {
    return std::nullopt;
  }
  EvidenceStamp stamp;
  stamp.provenance = provenance;
  stamp.observed_at = Timestamp::from_unix_millis(observed_at);
  stamp.freshness = freshness;
  stamp.ttl_millis = ttl_millis;
  return stamp;
}

void write_structured_error(WireWriter& w, const StructuredError& error) {
  write_enum(w, error.category);
  write_enum(w, error.stage);
  w.writer.text(error.subject);
  w.writer.text(error.reason);
  w.writer.text(error.expected);
  w.writer.text(error.current);
  w.writer.text(error.message);
}

[[nodiscard]] std::optional<StructuredError> read_structured_error(WireReader& w, const char* name) {
  StructuredError error;
  if (!read_enum(w, error.category, kMaxErrorCategoryRaw, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, error.stage, kMaxErrorStageRaw, name)) {
    return std::nullopt;
  }
  if (!read_string(w, error.subject, name)) {
    return std::nullopt;
  }
  if (!read_string(w, error.reason, name)) {
    return std::nullopt;
  }
  if (!read_string(w, error.expected, name)) {
    return std::nullopt;
  }
  if (!read_string(w, error.current, name)) {
    return std::nullopt;
  }
  if (!read_string(w, error.message, name)) {
    return std::nullopt;
  }
  return error;
}

void write_explanation_factor(WireWriter& w, const ExplanationFactor& factor) {
  w.writer.text(factor.code);
  w.writer.text(factor.subject);
  w.writer.text(factor.detail);
}

[[nodiscard]] std::optional<ExplanationFactor> read_explanation_factor(WireReader& w,
                                                                      const char* name) {
  ExplanationFactor factor;
  if (!read_string(w, factor.code, name)) {
    return std::nullopt;
  }
  if (!read_string(w, factor.subject, name)) {
    return std::nullopt;
  }
  if (!read_string(w, factor.detail, name)) {
    return std::nullopt;
  }
  return factor;
}

void write_explanation(WireWriter& w, const Explanation& explanation) {
  w.writer.text(explanation.code);
  w.writer.text(explanation.subject);
  w.writer.text(explanation.summary);
  write_vector(w, explanation.factors, kMaxExplanationFactors,
               [&w](const ExplanationFactor& factor) { write_explanation_factor(w, factor); });
}

[[nodiscard]] std::optional<Explanation> read_explanation(WireReader& w, const char* name) {
  Explanation explanation;
  if (!read_string(w, explanation.code, name)) {
    return std::nullopt;
  }
  if (!read_string(w, explanation.subject, name)) {
    return std::nullopt;
  }
  if (!read_string(w, explanation.summary, name)) {
    return std::nullopt;
  }
  if (!read_vector(w, explanation.factors, kMaxExplanationFactors, name, read_explanation_factor)) {
    return std::nullopt;
  }
  return explanation;
}

void write_readiness_contract(WireWriter& w, const ReadinessContract& contract) {
  w.writer.u64(static_cast<std::uint64_t>(contract.minimum_current_racks));
  write_identity_vector(w, contract.mandatory_racks, kMaxRacksPerCluster);
  w.writer.boolean(contract.require_all_active_racks_current);
  w.writer.boolean(contract.require_connectivity_evidence);
  w.writer.u64(static_cast<std::uint64_t>(contract.minimum_current_links));
  write_enum_vector(w, contract.required_placement_domain_classes, kMaxRequiredPlacementClasses);
  write_enum_vector(w, contract.required_failure_domain_classes, kMaxRequiredFailureClasses);
  w.writer.boolean(contract.allow_partial);
  w.writer.boolean(contract.allow_degraded);
  w.writer.boolean(contract.require_no_conflicts);
}

[[nodiscard]] std::optional<ReadinessContract> read_readiness_contract(WireReader& w,
                                                                      const char* name) {
  ReadinessContract contract;
  std::size_t minimum_current_racks = 0;
  if (!read_size(w, minimum_current_racks, name)) {
    return std::nullopt;
  }
  contract.minimum_current_racks = minimum_current_racks;
  if (!read_identity_vector(w, contract.mandatory_racks, kMaxRacksPerCluster, name)) {
    return std::nullopt;
  }
  if (!read_bool(w, contract.require_all_active_racks_current, name)) {
    return std::nullopt;
  }
  if (!read_bool(w, contract.require_connectivity_evidence, name)) {
    return std::nullopt;
  }
  std::size_t minimum_current_links = 0;
  if (!read_size(w, minimum_current_links, name)) {
    return std::nullopt;
  }
  contract.minimum_current_links = minimum_current_links;
  if (!read_enum_vector(w, contract.required_placement_domain_classes, kMaxPlacementDomainClassRaw,
                        kMaxRequiredPlacementClasses, name)) {
    return std::nullopt;
  }
  if (!read_enum_vector(w, contract.required_failure_domain_classes, kMaxFailureDomainClassRaw,
                        kMaxRequiredFailureClasses, name)) {
    return std::nullopt;
  }
  if (!read_bool(w, contract.allow_partial, name)) {
    return std::nullopt;
  }
  if (!read_bool(w, contract.allow_degraded, name)) {
    return std::nullopt;
  }
  if (!read_bool(w, contract.require_no_conflicts, name)) {
    return std::nullopt;
  }
  return contract;
}

void write_readiness_blocker(WireWriter& w, const ReadinessBlocker& blocker) {
  w.writer.text(blocker.code);
  w.writer.text(blocker.subject);
  w.writer.text(blocker.detail);
}

[[nodiscard]] std::optional<ReadinessBlocker> read_readiness_blocker(WireReader& w,
                                                                    const char* name) {
  ReadinessBlocker blocker;
  if (!read_string(w, blocker.code, name)) {
    return std::nullopt;
  }
  if (!read_string(w, blocker.subject, name)) {
    return std::nullopt;
  }
  if (!read_string(w, blocker.detail, name)) {
    return std::nullopt;
  }
  return blocker;
}

void write_readiness_evaluation(WireWriter& w, const ReadinessEvaluation& evaluation) {
  w.writer.boolean(evaluation.satisfied);
  write_enum(w, evaluation.lifecycle);
  write_vector(w, evaluation.blockers, kMaxDecodedRecords,
               [&w](const ReadinessBlocker& blocker) { write_readiness_blocker(w, blocker); });
}

[[nodiscard]] std::optional<ReadinessEvaluation> read_readiness_evaluation(WireReader& w,
                                                                          const char* name) {
  ReadinessEvaluation evaluation;
  if (!read_bool(w, evaluation.satisfied, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, evaluation.lifecycle, kMaxClusterLifecycleRaw, name)) {
    return std::nullopt;
  }
  if (!read_vector(w, evaluation.blockers, kMaxDecodedRecords, name, read_readiness_blocker)) {
    return std::nullopt;
  }
  return evaluation;
}

void write_domain_header(WireWriter& w, const DomainHeader& header) {
  write_evidence_stamp(w, header.evidence);
  write_counter(w, header.generation);
  write_optional_identity(w, header.publisher);
  write_counter(w, header.cluster_epoch);
  write_counter(w, header.coordinator_epoch);
  w.writer.text(header.label);
}

[[nodiscard]] std::optional<DomainHeader> read_domain_header(WireReader& w, const char* name) {
  DomainHeader header;
  std::optional<EvidenceStamp> evidence = read_evidence_stamp(w, name);
  if (!evidence.has_value()) {
    return std::nullopt;
  }
  header.evidence = *evidence;
  if (!read_counter(w, header.generation, name)) {
    return std::nullopt;
  }
  if (!read_optional_identity(w, header.publisher, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, header.cluster_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, header.coordinator_epoch, name)) {
    return std::nullopt;
  }
  if (!read_string(w, header.label, name)) {
    return std::nullopt;
  }
  return header;
}

// ---------------------------------------------------------------------------
// Domains
// ---------------------------------------------------------------------------

void write_placement_domain(WireWriter& w, const PlacementDomain& domain) {
  write_identity(w, domain.id);
  write_enum(w, domain.klass);
  write_identity_vector(w, domain.racks, kMaxDomainMembers);
  write_domain_header(w, domain.header);
}

[[nodiscard]] std::optional<PlacementDomain> read_placement_domain(WireReader& w, const char* name) {
  std::optional<PlacementDomainId> id = read_identity_value<PlacementDomainId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  PlacementDomainClass klass = PlacementDomainClass::Unknown;
  if (!read_enum(w, klass, kMaxPlacementDomainClassRaw, name)) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxDomainMembers, name)) {
    return std::nullopt;
  }
  std::optional<DomainHeader> header = read_domain_header(w, name);
  if (!header.has_value()) {
    return std::nullopt;
  }
  return PlacementDomain{std::move(*id), klass, std::move(racks), std::move(*header)};
}

void write_capacity_quantity(WireWriter& w, const CapacityQuantity& quantity) {
  w.writer.text(quantity.unit);
  write_optional_f64(w, quantity.value);
  write_enum(w, quantity.provenance);
  w.writer.boolean(quantity.aggregated);
}

[[nodiscard]] std::optional<CapacityQuantity> read_capacity_quantity(WireReader& w,
                                                                    const char* name) {
  CapacityQuantity quantity;
  if (!read_string(w, quantity.unit, name)) {
    return std::nullopt;
  }
  if (!read_optional_f64(w, quantity.value, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, quantity.provenance, kMaxEvidenceProvenanceRaw, name)) {
    return std::nullopt;
  }
  if (!read_bool(w, quantity.aggregated, name)) {
    return std::nullopt;
  }
  return quantity;
}

void write_capacity_domain(WireWriter& w, const CapacityDomain& domain) {
  write_identity(w, domain.id);
  write_enum(w, domain.klass);
  write_identity_vector(w, domain.racks, kMaxDomainMembers);
  write_vector(w, domain.quantities, kMaxCapacityQuantities,
               [&w](const CapacityQuantity& quantity) { write_capacity_quantity(w, quantity); });
  write_domain_header(w, domain.header);
}

[[nodiscard]] std::optional<CapacityDomain> read_capacity_domain(WireReader& w, const char* name) {
  std::optional<CapacityDomainId> id = read_identity_value<CapacityDomainId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  CapacityDomainClass klass = CapacityDomainClass::Unknown;
  if (!read_enum(w, klass, kMaxCapacityDomainClassRaw, name)) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxDomainMembers, name)) {
    return std::nullopt;
  }
  std::vector<CapacityQuantity> quantities;
  if (!read_vector(w, quantities, kMaxCapacityQuantities, name, read_capacity_quantity)) {
    return std::nullopt;
  }
  std::optional<DomainHeader> header = read_domain_header(w, name);
  if (!header.has_value()) {
    return std::nullopt;
  }
  return CapacityDomain{std::move(*id), klass, std::move(racks), std::move(quantities),
                        std::move(*header)};
}

void write_failure_domain(WireWriter& w, const FailureDomain& domain) {
  write_identity(w, domain.id);
  write_enum(w, domain.klass);
  write_identity_vector(w, domain.racks, kMaxDomainMembers);
  write_domain_header(w, domain.header);
}

[[nodiscard]] std::optional<FailureDomain> read_failure_domain(WireReader& w, const char* name) {
  std::optional<FailureDomainId> id = read_identity_value<FailureDomainId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  FailureDomainClass klass = FailureDomainClass::Unknown;
  if (!read_enum(w, klass, kMaxFailureDomainClassRaw, name)) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxDomainMembers, name)) {
    return std::nullopt;
  }
  std::optional<DomainHeader> header = read_domain_header(w, name);
  if (!header.has_value()) {
    return std::nullopt;
  }
  return FailureDomain{std::move(*id), klass, std::move(racks), std::move(*header)};
}

void write_network_domain(WireWriter& w, const NetworkDomain& domain) {
  write_identity(w, domain.id);
  write_enum(w, domain.connectivity);
  write_identity_vector(w, domain.racks, kMaxDomainMembers);
  write_domain_header(w, domain.header);
}

[[nodiscard]] std::optional<NetworkDomain> read_network_domain(WireReader& w, const char* name) {
  std::optional<NetworkDomainId> id = read_identity_value<NetworkDomainId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  ConnectivityClass connectivity = ConnectivityClass::Unknown;
  if (!read_enum(w, connectivity, kMaxConnectivityClassRaw, name)) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxDomainMembers, name)) {
    return std::nullopt;
  }
  std::optional<DomainHeader> header = read_domain_header(w, name);
  if (!header.has_value()) {
    return std::nullopt;
  }
  return NetworkDomain{std::move(*id), connectivity, std::move(racks), std::move(*header)};
}

void write_storage_domain(WireWriter& w, const StorageDomain& domain) {
  write_identity(w, domain.id);
  write_identity_vector(w, domain.racks, kMaxDomainMembers);
  write_domain_header(w, domain.header);
}

[[nodiscard]] std::optional<StorageDomain> read_storage_domain(WireReader& w, const char* name) {
  std::optional<StorageDomainId> id = read_identity_value<StorageDomainId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxDomainMembers, name)) {
    return std::nullopt;
  }
  std::optional<DomainHeader> header = read_domain_header(w, name);
  if (!header.has_value()) {
    return std::nullopt;
  }
  return StorageDomain{std::move(*id), std::move(racks), std::move(*header)};
}

void write_power_domain(WireWriter& w, const PowerDomain& domain) {
  write_identity(w, domain.id);
  write_identity_vector(w, domain.racks, kMaxDomainMembers);
  write_optional_identity(w, domain.parent);
  write_domain_header(w, domain.header);
}

[[nodiscard]] std::optional<PowerDomain> read_power_domain(WireReader& w, const char* name) {
  std::optional<PowerDomainId> id = read_identity_value<PowerDomainId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxDomainMembers, name)) {
    return std::nullopt;
  }
  std::optional<PowerDomainId> parent;
  if (!read_optional_identity(w, parent, name)) {
    return std::nullopt;
  }
  std::optional<DomainHeader> header = read_domain_header(w, name);
  if (!header.has_value()) {
    return std::nullopt;
  }
  return PowerDomain{std::move(*id), std::move(racks), std::move(parent), std::move(*header)};
}

void write_cooling_domain(WireWriter& w, const CoolingDomain& domain) {
  write_identity(w, domain.id);
  write_identity_vector(w, domain.racks, kMaxDomainMembers);
  write_optional_identity(w, domain.parent);
  write_domain_header(w, domain.header);
}

[[nodiscard]] std::optional<CoolingDomain> read_cooling_domain(WireReader& w, const char* name) {
  std::optional<CoolingDomainId> id = read_identity_value<CoolingDomainId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxDomainMembers, name)) {
    return std::nullopt;
  }
  std::optional<CoolingDomainId> parent;
  if (!read_optional_identity(w, parent, name)) {
    return std::nullopt;
  }
  std::optional<DomainHeader> header = read_domain_header(w, name);
  if (!header.has_value()) {
    return std::nullopt;
  }
  return CoolingDomain{std::move(*id), std::move(racks), std::move(parent), std::move(*header)};
}

void write_link_domain(WireWriter& w, const LinkDomain& domain) {
  write_identity(w, domain.id);
  write_identity_vector(w, domain.racks, kMaxDomainMembers);
  write_domain_header(w, domain.header);
}

[[nodiscard]] std::optional<LinkDomain> read_link_domain(WireReader& w, const char* name) {
  std::optional<LinkDomainId> id = read_identity_value<LinkDomainId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxDomainMembers, name)) {
    return std::nullopt;
  }
  std::optional<DomainHeader> header = read_domain_header(w, name);
  if (!header.has_value()) {
    return std::nullopt;
  }
  return LinkDomain{std::move(*id), std::move(racks), std::move(*header)};
}

void write_constraint(WireWriter& w, const ClusterConstraint& constraint) {
  write_identity(w, constraint.id);
  write_enum(w, constraint.kind);
  write_identity_vector(w, constraint.racks, kMaxDomainMembers);
  w.writer.text(constraint.domain_ref);
  w.writer.text(constraint.statement);
  write_domain_header(w, constraint.header);
}

[[nodiscard]] std::optional<ClusterConstraint> read_constraint(WireReader& w, const char* name) {
  std::optional<ConstraintId> id = read_identity_value<ConstraintId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  ConstraintKind kind = ConstraintKind::Unknown;
  if (!read_enum(w, kind, kMaxConstraintKindRaw, name)) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxDomainMembers, name)) {
    return std::nullopt;
  }
  std::string domain_ref;
  if (!read_string(w, domain_ref, name)) {
    return std::nullopt;
  }
  std::string statement;
  if (!read_string(w, statement, name)) {
    return std::nullopt;
  }
  std::optional<DomainHeader> header = read_domain_header(w, name);
  if (!header.has_value()) {
    return std::nullopt;
  }
  return ClusterConstraint{std::move(*id),
                           kind,
                           std::move(racks),
                           std::move(domain_ref),
                           std::move(statement),
                           std::move(*header)};
}

// ---------------------------------------------------------------------------
// Topology
// ---------------------------------------------------------------------------

void write_topology_record(WireWriter& w, const TopologyEpochRecord& record) {
  write_counter(w, record.epoch);
  write_counter(w, record.generation);
  write_timestamp(w, record.established_at);
  write_evidence_stamp(w, record.evidence);
  w.writer.text(record.reason);
}

[[nodiscard]] std::optional<TopologyEpochRecord> read_topology_record(WireReader& w,
                                                                     const char* name) {
  TopologyEpochRecord record;
  if (!read_counter(w, record.epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, record.generation, name)) {
    return std::nullopt;
  }
  std::int64_t established_at = 0;
  if (!read_i64(w, established_at, name)) {
    return std::nullopt;
  }
  record.established_at = Timestamp::from_unix_millis(established_at);
  std::optional<EvidenceStamp> evidence = read_evidence_stamp(w, name);
  if (!evidence.has_value()) {
    return std::nullopt;
  }
  record.evidence = *evidence;
  if (!read_string(w, record.reason, name)) {
    return std::nullopt;
  }
  return record;
}

void write_inter_rack_link(WireWriter& w, const InterRackLink& link) {
  write_identity(w, link.id);
  write_identity(w, link.source);
  write_identity(w, link.destination);
  write_enum(w, link.direction);
  write_enum(w, link.connectivity);
  write_optional_identity(w, link.network_domain);
  write_optional_identity(w, link.link_domain);
  write_optional_identity(w, link.source_endpoint);
  write_optional_identity(w, link.destination_endpoint);
  write_enum(w, link.bandwidth_class);
  write_optional_u64(w, link.nominal_bandwidth_bps);
  write_enum(w, link.latency_class);
  write_optional_u64(w, link.nominal_latency_nanos);
  write_optional_u32(w, link.hop_count);
  write_enum(w, link.reachability);
  write_enum(w, link.health);
  write_identity_vector(w, link.failure_domains, kMaxFailureDomainRefs);
  write_domain_header(w, link.header);
  write_counter(w, link.topology_epoch);
  write_counter(w, link.topology_generation);
  write_counter(w, link.publication);
  write_optional_identity(w, link.boot);
}

[[nodiscard]] std::optional<InterRackLink> read_inter_rack_link(WireReader& w, const char* name) {
  std::optional<InterRackLinkId> id = read_identity_value<InterRackLinkId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  std::optional<RackId> source = read_identity_value<RackId>(w, name);
  if (!source.has_value()) {
    return std::nullopt;
  }
  std::optional<RackId> destination = read_identity_value<RackId>(w, name);
  if (!destination.has_value()) {
    return std::nullopt;
  }
  LinkDirection direction = LinkDirection::Unknown;
  if (!read_enum(w, direction, kMaxLinkDirectionRaw, name)) {
    return std::nullopt;
  }
  ConnectivityClass connectivity = ConnectivityClass::Unknown;
  if (!read_enum(w, connectivity, kMaxConnectivityClassRaw, name)) {
    return std::nullopt;
  }
  std::optional<NetworkDomainId> network_domain;
  if (!read_optional_identity(w, network_domain, name)) {
    return std::nullopt;
  }
  std::optional<LinkDomainId> link_domain;
  if (!read_optional_identity(w, link_domain, name)) {
    return std::nullopt;
  }
  std::optional<RackEndpointId> source_endpoint;
  if (!read_optional_identity(w, source_endpoint, name)) {
    return std::nullopt;
  }
  std::optional<RackEndpointId> destination_endpoint;
  if (!read_optional_identity(w, destination_endpoint, name)) {
    return std::nullopt;
  }
  BandwidthClass bandwidth_class = BandwidthClass::Unknown;
  if (!read_enum(w, bandwidth_class, kMaxBandwidthClassRaw, name)) {
    return std::nullopt;
  }
  std::optional<std::uint64_t> nominal_bandwidth_bps;
  if (!read_optional_u64(w, nominal_bandwidth_bps, name)) {
    return std::nullopt;
  }
  LatencyClass latency_class = LatencyClass::Unknown;
  if (!read_enum(w, latency_class, kMaxLatencyClassRaw, name)) {
    return std::nullopt;
  }
  std::optional<std::uint64_t> nominal_latency_nanos;
  if (!read_optional_u64(w, nominal_latency_nanos, name)) {
    return std::nullopt;
  }
  std::optional<std::uint32_t> hop_count;
  if (!read_optional_u32(w, hop_count, name)) {
    return std::nullopt;
  }
  Reachability reachability = Reachability::Unknown;
  if (!read_enum(w, reachability, kMaxReachabilityRaw, name)) {
    return std::nullopt;
  }
  HealthState health = HealthState::Unknown;
  if (!read_enum(w, health, kMaxHealthStateRaw, name)) {
    return std::nullopt;
  }
  std::vector<FailureDomainId> failure_domains;
  if (!read_identity_vector(w, failure_domains, kMaxFailureDomainRefs, name)) {
    return std::nullopt;
  }
  std::optional<DomainHeader> header = read_domain_header(w, name);
  if (!header.has_value()) {
    return std::nullopt;
  }
  std::optional<TopologyEpoch> topology_epoch = read_counter_value<TopologyEpoch>(w, name);
  if (!topology_epoch.has_value()) {
    return std::nullopt;
  }
  std::optional<TopologyGeneration> topology_generation =
      read_counter_value<TopologyGeneration>(w, name);
  if (!topology_generation.has_value()) {
    return std::nullopt;
  }
  std::optional<PublicationGeneration> publication =
      read_counter_value<PublicationGeneration>(w, name);
  if (!publication.has_value()) {
    return std::nullopt;
  }
  std::optional<RackAgentBootId> boot;
  if (!read_optional_identity(w, boot, name)) {
    return std::nullopt;
  }
  return InterRackLink{std::move(*id),
                       std::move(*source),
                       std::move(*destination),
                       direction,
                       connectivity,
                       std::move(network_domain),
                       std::move(link_domain),
                       std::move(source_endpoint),
                       std::move(destination_endpoint),
                       bandwidth_class,
                       std::move(nominal_bandwidth_bps),
                       latency_class,
                       std::move(nominal_latency_nanos),
                       std::move(hop_count),
                       reachability,
                       health,
                       std::move(failure_domains),
                       std::move(*header),
                       *topology_epoch,
                       *topology_generation,
                       *publication,
                       std::move(boot)};
}

// ---------------------------------------------------------------------------
// Rack reference
// ---------------------------------------------------------------------------

void write_accelerator_summary(WireWriter& w, const AcceleratorClassSummary& summary) {
  write_enum(w, summary.vendor);
  w.writer.text(summary.family);
  write_optional_u32(w, summary.device_count);
  write_optional_u64(w, summary.device_memory_bytes);
}

[[nodiscard]] std::optional<AcceleratorClassSummary> read_accelerator_summary(WireReader& w,
                                                                             const char* name) {
  AcceleratorClassSummary summary;
  if (!read_enum(w, summary.vendor, kMaxAcceleratorVendorRaw, name)) {
    return std::nullopt;
  }
  if (!read_string(w, summary.family, name)) {
    return std::nullopt;
  }
  if (!read_optional_u32(w, summary.device_count, name)) {
    return std::nullopt;
  }
  if (!read_optional_u64(w, summary.device_memory_bytes, name)) {
    return std::nullopt;
  }
  return summary;
}

void write_composition(WireWriter& w, const RackCompositionSummary& composition) {
  write_vector(w, composition.accelerators, kMaxAcceleratorClasses,
               [&w](const AcceleratorClassSummary& summary) {
                 write_accelerator_summary(w, summary);
               });
  write_optional_u32(w, composition.cpu_sockets);
  write_optional_u32(w, composition.cpu_cores);
  write_optional_u64(w, composition.host_memory_bytes);
  write_optional_u32(w, composition.nic_count);
  write_optional_u32(w, composition.switch_count);
  w.writer.text(composition.composition_label);
  write_enum(w, composition.provenance);
}

[[nodiscard]] std::optional<RackCompositionSummary> read_composition(WireReader& w,
                                                                    const char* name) {
  RackCompositionSummary composition;
  if (!read_vector(w, composition.accelerators, kMaxAcceleratorClasses, name,
                   read_accelerator_summary)) {
    return std::nullopt;
  }
  if (!read_optional_u32(w, composition.cpu_sockets, name)) {
    return std::nullopt;
  }
  if (!read_optional_u32(w, composition.cpu_cores, name)) {
    return std::nullopt;
  }
  if (!read_optional_u64(w, composition.host_memory_bytes, name)) {
    return std::nullopt;
  }
  if (!read_optional_u32(w, composition.nic_count, name)) {
    return std::nullopt;
  }
  if (!read_optional_u32(w, composition.switch_count, name)) {
    return std::nullopt;
  }
  if (!read_string(w, composition.composition_label, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, composition.provenance, kMaxEvidenceProvenanceRaw, name)) {
    return std::nullopt;
  }
  return composition;
}

void write_rack_endpoint(WireWriter& w, const RackEndpoint& endpoint) {
  write_identity(w, endpoint.id);
  write_optional_identity(w, endpoint.network_domain);
  write_optional_identity(w, endpoint.link_domain);
  write_enum(w, endpoint.connectivity);
  write_evidence_stamp(w, endpoint.evidence);
}

[[nodiscard]] std::optional<RackEndpoint> read_rack_endpoint(WireReader& w, const char* name) {
  std::optional<RackEndpointId> id = read_identity_value<RackEndpointId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  std::optional<NetworkDomainId> network_domain;
  if (!read_optional_identity(w, network_domain, name)) {
    return std::nullopt;
  }
  std::optional<LinkDomainId> link_domain;
  if (!read_optional_identity(w, link_domain, name)) {
    return std::nullopt;
  }
  ConnectivityClass connectivity = ConnectivityClass::Unknown;
  if (!read_enum(w, connectivity, kMaxConnectivityClassRaw, name)) {
    return std::nullopt;
  }
  std::optional<EvidenceStamp> evidence = read_evidence_stamp(w, name);
  if (!evidence.has_value()) {
    return std::nullopt;
  }
  return RackEndpoint{std::move(*id), std::move(network_domain), std::move(link_domain),
                      connectivity, std::move(*evidence)};
}

void write_failure_hint(WireWriter& w, const RackFailureDomainHint& hint) {
  write_enum(w, hint.klass);
  write_identity(w, hint.id);
  write_evidence_stamp(w, hint.evidence);
}

[[nodiscard]] std::optional<RackFailureDomainHint> read_failure_hint(WireReader& w,
                                                                    const char* name) {
  FailureDomainClass klass = FailureDomainClass::Unknown;
  if (!read_enum(w, klass, kMaxFailureDomainClassRaw, name)) {
    return std::nullopt;
  }
  std::optional<FailureDomainId> id = read_identity_value<FailureDomainId>(w, name);
  if (!id.has_value()) {
    return std::nullopt;
  }
  std::optional<EvidenceStamp> evidence = read_evidence_stamp(w, name);
  if (!evidence.has_value()) {
    return std::nullopt;
  }
  return RackFailureDomainHint{klass, std::move(*id), std::move(*evidence)};
}

void write_rack_reference(WireWriter& w, const RackReference& reference) {
  write_identity(w, reference.rack);
  write_counter(w, reference.generation);
  write_enum(w, reference.rack_lifecycle);
  write_enum(w, reference.currentness);
  write_composition(w, reference.composition);
  write_vector(w, reference.endpoints, kMaxRackEndpoints,
               [&w](const RackEndpoint& endpoint) { write_rack_endpoint(w, endpoint); });
  write_vector(w, reference.failure_domain_hints, kMaxFailureDomainRefs,
               [&w](const RackFailureDomainHint& hint) { write_failure_hint(w, hint); });
  write_evidence_stamp(w, reference.evidence);
  write_optional_identity(w, reference.publisher);
  write_counter(w, reference.publication);
  write_identity(w, reference.boot);
  write_counter(w, reference.cluster_epoch);
  write_counter(w, reference.coordinator_epoch);
  write_enum(w, reference.health);
  w.writer.text(reference.origin_label);
}

[[nodiscard]] std::optional<RackReference> read_rack_reference(WireReader& w, const char* name) {
  std::optional<RackId> rack = read_identity_value<RackId>(w, name);
  if (!rack.has_value()) {
    return std::nullopt;
  }
  std::optional<RackGeneration> generation = read_counter_value<RackGeneration>(w, name);
  if (!generation.has_value()) {
    return std::nullopt;
  }
  RackLifecycleState rack_lifecycle = RackLifecycleState::Unknown;
  if (!read_enum(w, rack_lifecycle, kMaxRackLifecycleStateRaw, name)) {
    return std::nullopt;
  }
  RackCurrentness currentness = RackCurrentness::Unknown;
  if (!read_enum(w, currentness, kMaxRackCurrentnessRaw, name)) {
    return std::nullopt;
  }
  std::optional<RackCompositionSummary> composition = read_composition(w, name);
  if (!composition.has_value()) {
    return std::nullopt;
  }
  std::vector<RackEndpoint> endpoints;
  if (!read_vector(w, endpoints, kMaxRackEndpoints, name, read_rack_endpoint)) {
    return std::nullopt;
  }
  std::vector<RackFailureDomainHint> failure_domain_hints;
  if (!read_vector(w, failure_domain_hints, kMaxFailureDomainRefs, name, read_failure_hint)) {
    return std::nullopt;
  }
  std::optional<EvidenceStamp> evidence = read_evidence_stamp(w, name);
  if (!evidence.has_value()) {
    return std::nullopt;
  }
  std::optional<RackPublisherId> publisher;
  if (!read_optional_identity(w, publisher, name)) {
    return std::nullopt;
  }
  std::optional<PublicationGeneration> publication =
      read_counter_value<PublicationGeneration>(w, name);
  if (!publication.has_value()) {
    return std::nullopt;
  }
  std::optional<RackAgentBootId> boot = read_identity_value<RackAgentBootId>(w, name);
  if (!boot.has_value()) {
    return std::nullopt;
  }
  std::optional<ClusterEpoch> cluster_epoch = read_counter_value<ClusterEpoch>(w, name);
  if (!cluster_epoch.has_value()) {
    return std::nullopt;
  }
  std::optional<CoordinatorEpoch> coordinator_epoch = read_counter_value<CoordinatorEpoch>(w, name);
  if (!coordinator_epoch.has_value()) {
    return std::nullopt;
  }
  HealthState health = HealthState::Unknown;
  if (!read_enum(w, health, kMaxHealthStateRaw, name)) {
    return std::nullopt;
  }
  std::string origin_label;
  if (!read_string(w, origin_label, name)) {
    return std::nullopt;
  }
  return RackReference{std::move(*rack),
                       *generation,
                       rack_lifecycle,
                       currentness,
                       std::move(*composition),
                       std::move(endpoints),
                       std::move(failure_domain_hints),
                       std::move(*evidence),
                       std::move(publisher),
                       *publication,
                       std::move(*boot),
                       *cluster_epoch,
                       *coordinator_epoch,
                       health,
                       std::move(origin_label)};
}

// ---------------------------------------------------------------------------
// Mutation request and result
// ---------------------------------------------------------------------------

void write_authority(WireWriter& w, const MutationAuthority& authority) {
  write_counter(w, authority.cluster_epoch);
  write_counter(w, authority.coordinator_epoch);
  write_counter(w, authority.topology_epoch);
  write_counter(w, authority.topology_generation);
  write_optional_identity(w, authority.boot);
  write_optional_identity(w, authority.publisher);
  write_optional_identity(w, authority.rack);
  write_optional_counter(w, authority.rack_generation);
  write_counter(w, authority.publication);
}

[[nodiscard]] std::optional<MutationAuthority> read_authority(WireReader& w, const char* name) {
  MutationAuthority authority;
  if (!read_counter(w, authority.cluster_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, authority.coordinator_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, authority.topology_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, authority.topology_generation, name)) {
    return std::nullopt;
  }
  if (!read_optional_identity(w, authority.boot, name)) {
    return std::nullopt;
  }
  if (!read_optional_identity(w, authority.publisher, name)) {
    return std::nullopt;
  }
  if (!read_optional_identity(w, authority.rack, name)) {
    return std::nullopt;
  }
  if (!read_optional_counter(w, authority.rack_generation, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, authority.publication, name)) {
    return std::nullopt;
  }
  return authority;
}

void write_mutation_request(WireWriter& w, const MutationRequest& request) {
  write_enum(w, request.kind);
  write_identity(w, request.cluster);
  write_authority(w, request.authority);
  write_evidence_stamp(w, request.evidence);
  w.writer.text(request.reason);
  write_timestamp(w, request.requested_at);
  write_enum(w, request.declared_lifecycle);
  write_readiness_contract(w, request.readiness_contract);
  write_rack_reference(w, request.rack_reference);
  write_enum(w, request.membership);
  write_optional_counter(w, request.expected_rack_generation);
  write_optional_counter(w, request.new_rack_generation);
  write_optional_record(w, request.placement_domain, write_placement_domain);
  write_optional_record(w, request.capacity_domain, write_capacity_domain);
  write_optional_record(w, request.failure_domain, write_failure_domain);
  write_optional_record(w, request.network_domain, write_network_domain);
  write_optional_record(w, request.storage_domain, write_storage_domain);
  write_optional_record(w, request.power_domain, write_power_domain);
  write_optional_record(w, request.cooling_domain, write_cooling_domain);
  write_optional_record(w, request.link_domain, write_link_domain);
  write_optional_record(w, request.constraint, write_constraint);
  write_optional_record(w, request.link, write_inter_rack_link);
  write_optional_identity(w, request.link_id);
  write_optional_counter(w, request.target_topology_epoch);
  w.writer.text(request.topology_reason);
  write_enum(w, request.health);
  write_enum(w, request.reachability);
  w.writer.text(request.evidence_selector);
  // Target rack list for rack-scoped mutations such as
  // REVALIDATE_RECOVERED_STATE. Empty means every member rack.
  write_identity_vector(w, request.racks, kMaxRacksPerCluster);
}

[[nodiscard]] std::optional<MutationRequest> read_mutation_request(WireReader& w,
                                                                  const char* name) {
  MutationKind kind = MutationKind::Unknown;
  if (!read_enum(w, kind, kMaxMutationKindRaw, name)) {
    return std::nullopt;
  }
  std::optional<ClusterId> cluster = read_identity_value<ClusterId>(w, name);
  if (!cluster.has_value()) {
    return std::nullopt;
  }
  std::optional<MutationAuthority> authority = read_authority(w, name);
  if (!authority.has_value()) {
    return std::nullopt;
  }
  std::optional<EvidenceStamp> evidence = read_evidence_stamp(w, name);
  if (!evidence.has_value()) {
    return std::nullopt;
  }
  std::string reason;
  if (!read_string(w, reason, name)) {
    return std::nullopt;
  }
  std::int64_t requested_at = 0;
  if (!read_i64(w, requested_at, name)) {
    return std::nullopt;
  }
  ClusterLifecycle declared_lifecycle = ClusterLifecycle::Declared;
  if (!read_enum(w, declared_lifecycle, kMaxClusterLifecycleRaw, name)) {
    return std::nullopt;
  }
  std::optional<ReadinessContract> readiness_contract = read_readiness_contract(w, name);
  if (!readiness_contract.has_value()) {
    return std::nullopt;
  }
  std::optional<RackReference> rack_reference = read_rack_reference(w, name);
  if (!rack_reference.has_value()) {
    return std::nullopt;
  }
  RackMembershipState membership = RackMembershipState::Unknown;
  if (!read_enum(w, membership, kMaxRackMembershipStateRaw, name)) {
    return std::nullopt;
  }
  std::optional<RackGeneration> expected_rack_generation;
  if (!read_optional_counter(w, expected_rack_generation, name)) {
    return std::nullopt;
  }
  std::optional<RackGeneration> new_rack_generation;
  if (!read_optional_counter(w, new_rack_generation, name)) {
    return std::nullopt;
  }
  std::optional<PlacementDomain> placement_domain;
  if (!read_optional_record(w, placement_domain, name, read_placement_domain)) {
    return std::nullopt;
  }
  std::optional<CapacityDomain> capacity_domain;
  if (!read_optional_record(w, capacity_domain, name, read_capacity_domain)) {
    return std::nullopt;
  }
  std::optional<FailureDomain> failure_domain;
  if (!read_optional_record(w, failure_domain, name, read_failure_domain)) {
    return std::nullopt;
  }
  std::optional<NetworkDomain> network_domain;
  if (!read_optional_record(w, network_domain, name, read_network_domain)) {
    return std::nullopt;
  }
  std::optional<StorageDomain> storage_domain;
  if (!read_optional_record(w, storage_domain, name, read_storage_domain)) {
    return std::nullopt;
  }
  std::optional<PowerDomain> power_domain;
  if (!read_optional_record(w, power_domain, name, read_power_domain)) {
    return std::nullopt;
  }
  std::optional<CoolingDomain> cooling_domain;
  if (!read_optional_record(w, cooling_domain, name, read_cooling_domain)) {
    return std::nullopt;
  }
  std::optional<LinkDomain> link_domain;
  if (!read_optional_record(w, link_domain, name, read_link_domain)) {
    return std::nullopt;
  }
  std::optional<ClusterConstraint> constraint;
  if (!read_optional_record(w, constraint, name, read_constraint)) {
    return std::nullopt;
  }
  std::optional<InterRackLink> link;
  if (!read_optional_record(w, link, name, read_inter_rack_link)) {
    return std::nullopt;
  }
  std::optional<InterRackLinkId> link_id;
  bool link_id_present = false;
  if (!read_present(w, link_id_present, name)) {
    return std::nullopt;
  }
  if (link_id_present) {
    link_id = read_identity_value<InterRackLinkId>(w, name);
    if (!link_id.has_value()) {
      return std::nullopt;
    }
  }
  std::optional<TopologyEpoch> target_topology_epoch;
  if (!read_optional_counter(w, target_topology_epoch, name)) {
    return std::nullopt;
  }
  std::string topology_reason;
  if (!read_string(w, topology_reason, name)) {
    return std::nullopt;
  }
  HealthState health = HealthState::Unknown;
  if (!read_enum(w, health, kMaxHealthStateRaw, name)) {
    return std::nullopt;
  }
  Reachability reachability = Reachability::Unknown;
  if (!read_enum(w, reachability, kMaxReachabilityRaw, name)) {
    return std::nullopt;
  }
  std::string evidence_selector;
  if (!read_string(w, evidence_selector, name)) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxRacksPerCluster, name)) {
    return std::nullopt;
  }
  return MutationRequest{kind,
                         std::move(*cluster),
                         std::move(*authority),
                         std::move(*evidence),
                         std::move(reason),
                         Timestamp::from_unix_millis(requested_at),
                         declared_lifecycle,
                         std::move(*readiness_contract),
                         std::move(*rack_reference),
                         membership,
                         std::move(expected_rack_generation),
                         std::move(new_rack_generation),
                         std::move(placement_domain),
                         std::move(capacity_domain),
                         std::move(failure_domain),
                         std::move(network_domain),
                         std::move(storage_domain),
                         std::move(power_domain),
                         std::move(cooling_domain),
                         std::move(link_domain),
                         std::move(constraint),
                         std::move(link),
                         std::move(link_id),
                         std::move(target_topology_epoch),
                         std::move(topology_reason),
                         health,
                         reachability,
                         std::move(evidence_selector),
                         std::move(racks)};
}

void write_mutation_result(WireWriter& w, const MutationResult& result) {
  write_enum(w, result.outcome);
  write_enum(w, result.reason);
  write_structured_error(w, result.error);
  write_explanation(w, result.explanation);
  write_counter(w, result.cluster_epoch);
  write_counter(w, result.coordinator_epoch);
  write_counter(w, result.cluster_generation);
  write_counter(w, result.membership_generation);
  write_counter(w, result.topology_epoch);
  write_counter(w, result.topology_generation);
  write_counter(w, result.connectivity_generation);
  write_counter(w, result.health_generation);
  write_counter(w, result.constraint_generation);
  write_counter(w, result.snapshot_generation);
  write_counter(w, result.publication_generation);
  write_enum(w, result.lifecycle);
}

[[nodiscard]] std::optional<MutationResult> read_mutation_result(WireReader& w, const char* name) {
  MutationResult result;
  if (!read_enum(w, result.outcome, kMaxMutationOutcomeRaw, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, result.reason, kMaxRejectionReasonRaw, name)) {
    return std::nullopt;
  }
  std::optional<StructuredError> error = read_structured_error(w, name);
  if (!error.has_value()) {
    return std::nullopt;
  }
  result.error = std::move(*error);
  std::optional<Explanation> explanation = read_explanation(w, name);
  if (!explanation.has_value()) {
    return std::nullopt;
  }
  result.explanation = std::move(*explanation);
  if (!read_counter(w, result.cluster_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, result.coordinator_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, result.cluster_generation, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, result.membership_generation, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, result.topology_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, result.topology_generation, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, result.connectivity_generation, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, result.health_generation, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, result.constraint_generation, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, result.snapshot_generation, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, result.publication_generation, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, result.lifecycle, kMaxClusterLifecycleRaw, name)) {
    return std::nullopt;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Snapshot view and validation
// ---------------------------------------------------------------------------

void write_domain_generations(WireWriter& w, const DomainGenerations& generations) {
  write_counter(w, generations.placement);
  write_counter(w, generations.capacity);
  write_counter(w, generations.failure);
  write_counter(w, generations.network);
  write_counter(w, generations.storage);
  write_counter(w, generations.power);
  write_counter(w, generations.cooling);
  write_counter(w, generations.link);
}

[[nodiscard]] bool read_domain_generations(WireReader& w, DomainGenerations& out,
                                           const char* name) {
  if (!read_counter(w, out.placement, name)) {
    return false;
  }
  if (!read_counter(w, out.capacity, name)) {
    return false;
  }
  if (!read_counter(w, out.failure, name)) {
    return false;
  }
  if (!read_counter(w, out.network, name)) {
    return false;
  }
  if (!read_counter(w, out.storage, name)) {
    return false;
  }
  if (!read_counter(w, out.power, name)) {
    return false;
  }
  if (!read_counter(w, out.cooling, name)) {
    return false;
  }
  if (!read_counter(w, out.link, name)) {
    return false;
  }
  return true;
}

void write_rack_binding(WireWriter& w, const RackGenerationBinding& binding) {
  write_identity(w, binding.rack);
  write_counter(w, binding.generation);
  write_enum(w, binding.membership);
  write_enum(w, binding.currentness);
  w.writer.boolean(binding.authoritative_current);
  write_identity(w, binding.boot);
}

[[nodiscard]] std::optional<RackGenerationBinding> read_rack_binding(WireReader& w,
                                                                    const char* name) {
  std::optional<RackId> rack = read_identity_value<RackId>(w, name);
  if (!rack.has_value()) {
    return std::nullopt;
  }
  std::optional<RackGeneration> generation = read_counter_value<RackGeneration>(w, name);
  if (!generation.has_value()) {
    return std::nullopt;
  }
  RackMembershipState membership = RackMembershipState::Unknown;
  if (!read_enum(w, membership, kMaxRackMembershipStateRaw, name)) {
    return std::nullopt;
  }
  RackCurrentness currentness = RackCurrentness::Unknown;
  if (!read_enum(w, currentness, kMaxRackCurrentnessRaw, name)) {
    return std::nullopt;
  }
  bool authoritative_current = false;
  if (!read_bool(w, authoritative_current, name)) {
    return std::nullopt;
  }
  std::optional<RackAgentBootId> boot = read_identity_value<RackAgentBootId>(w, name);
  if (!boot.has_value()) {
    return std::nullopt;
  }
  return RackGenerationBinding{std::move(*rack), *generation, membership, currentness,
                               authoritative_current, std::move(*boot)};
}

void write_snapshot_view(WireWriter& w, const SnapshotView& view) {
  write_identity(w, view.cluster);
  write_counter(w, view.cluster_epoch);
  write_counter(w, view.coordinator_epoch);
  write_counter(w, view.cluster_generation);
  write_counter(w, view.membership_generation);
  write_counter(w, view.topology_epoch);
  write_counter(w, view.topology_generation);
  write_counter(w, view.connectivity_generation);
  write_counter(w, view.health_generation);
  write_counter(w, view.constraint_generation);
  write_domain_generations(w, view.domain_generations);
  write_counter(w, view.snapshot_generation);
  write_counter(w, view.publication_generation);
  write_enum(w, view.lifecycle);
  write_vector(w, view.rack_bindings, kMaxSnapshotRackRefs,
               [&w](const RackGenerationBinding& binding) { write_rack_binding(w, binding); });
  w.writer.text(view.semantic_digest);
  w.writer.u64(static_cast<std::uint64_t>(view.rack_count));
  w.writer.u64(static_cast<std::uint64_t>(view.link_count));
  w.writer.u64(static_cast<std::uint64_t>(view.domain_count));
}

[[nodiscard]] std::optional<SnapshotView> read_snapshot_view(WireReader& w, const char* name) {
  std::optional<ClusterId> cluster = read_identity_value<ClusterId>(w, name);
  if (!cluster.has_value()) {
    return std::nullopt;
  }
  std::optional<ClusterEpoch> cluster_epoch = read_counter_value<ClusterEpoch>(w, name);
  if (!cluster_epoch.has_value()) {
    return std::nullopt;
  }
  std::optional<CoordinatorEpoch> coordinator_epoch = read_counter_value<CoordinatorEpoch>(w, name);
  if (!coordinator_epoch.has_value()) {
    return std::nullopt;
  }
  std::optional<ClusterGeneration> cluster_generation =
      read_counter_value<ClusterGeneration>(w, name);
  if (!cluster_generation.has_value()) {
    return std::nullopt;
  }
  std::optional<MembershipGeneration> membership_generation =
      read_counter_value<MembershipGeneration>(w, name);
  if (!membership_generation.has_value()) {
    return std::nullopt;
  }
  std::optional<TopologyEpoch> topology_epoch = read_counter_value<TopologyEpoch>(w, name);
  if (!topology_epoch.has_value()) {
    return std::nullopt;
  }
  std::optional<TopologyGeneration> topology_generation =
      read_counter_value<TopologyGeneration>(w, name);
  if (!topology_generation.has_value()) {
    return std::nullopt;
  }
  std::optional<ConnectivityGeneration> connectivity_generation =
      read_counter_value<ConnectivityGeneration>(w, name);
  if (!connectivity_generation.has_value()) {
    return std::nullopt;
  }
  std::optional<HealthGeneration> health_generation = read_counter_value<HealthGeneration>(w, name);
  if (!health_generation.has_value()) {
    return std::nullopt;
  }
  std::optional<ConstraintGeneration> constraint_generation =
      read_counter_value<ConstraintGeneration>(w, name);
  if (!constraint_generation.has_value()) {
    return std::nullopt;
  }
  DomainGenerations domain_generations;
  if (!read_domain_generations(w, domain_generations, name)) {
    return std::nullopt;
  }
  std::optional<SnapshotGeneration> snapshot_generation =
      read_counter_value<SnapshotGeneration>(w, name);
  if (!snapshot_generation.has_value()) {
    return std::nullopt;
  }
  std::optional<PublicationGeneration> publication_generation =
      read_counter_value<PublicationGeneration>(w, name);
  if (!publication_generation.has_value()) {
    return std::nullopt;
  }
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;
  if (!read_enum(w, lifecycle, kMaxClusterLifecycleRaw, name)) {
    return std::nullopt;
  }
  std::vector<RackGenerationBinding> rack_bindings;
  if (!read_vector(w, rack_bindings, kMaxSnapshotRackRefs, name, read_rack_binding)) {
    return std::nullopt;
  }
  std::string semantic_digest;
  if (!read_string(w, semantic_digest, name)) {
    return std::nullopt;
  }
  std::size_t rack_count = 0;
  if (!read_size(w, rack_count, name)) {
    return std::nullopt;
  }
  std::size_t link_count = 0;
  if (!read_size(w, link_count, name)) {
    return std::nullopt;
  }
  std::size_t domain_count = 0;
  if (!read_size(w, domain_count, name)) {
    return std::nullopt;
  }
  return SnapshotView{std::move(*cluster),
                      *cluster_epoch,
                      *coordinator_epoch,
                      *cluster_generation,
                      *membership_generation,
                      *topology_epoch,
                      *topology_generation,
                      *connectivity_generation,
                      *health_generation,
                      *constraint_generation,
                      domain_generations,
                      *snapshot_generation,
                      *publication_generation,
                      lifecycle,
                      std::move(rack_bindings),
                      std::move(semantic_digest),
                      rack_count,
                      link_count,
                      domain_count};
}

void write_snapshot_validation(WireWriter& w, const SnapshotValidation& validation) {
  w.writer.boolean(validation.current);
  w.writer.boolean(validation.consumable);
  write_enum_vector(w, validation.reasons, kMaxStaleReasons);
  write_vector(w, validation.subjects, kMaxDecodedRecords,
               [&w](const std::string& subject) { w.writer.text(subject); });
  write_explanation(w, validation.explanation);
}

[[nodiscard]] std::optional<SnapshotValidation> read_snapshot_validation(WireReader& w,
                                                                        const char* name) {
  SnapshotValidation validation;
  if (!read_bool(w, validation.current, name)) {
    return std::nullopt;
  }
  if (!read_bool(w, validation.consumable, name)) {
    return std::nullopt;
  }
  if (!read_enum_vector(w, validation.reasons, kMaxSnapshotStaleReasonRaw, kMaxStaleReasons,
                        name)) {
    return std::nullopt;
  }
  if (!read_string_vector(w, validation.subjects, kMaxDecodedRecords, name)) {
    return std::nullopt;
  }
  std::optional<Explanation> explanation = read_explanation(w, name);
  if (!explanation.has_value()) {
    return std::nullopt;
  }
  validation.explanation = std::move(*explanation);
  return validation;
}

// ---------------------------------------------------------------------------
// Message bodies
// ---------------------------------------------------------------------------

void write_hello(WireWriter& w, const HelloMessage& message) {
  w.writer.u16(message.protocol_version);
  write_identity(w, message.cluster);
  write_identity(w, message.boot);
  w.writer.text(message.role);
  w.writer.text(message.build);
}

[[nodiscard]] std::optional<HelloMessage> read_hello(WireReader& w, const char* name) {
  std::uint16_t protocol_version = 0;
  if (!read_u16(w, protocol_version, name)) {
    return std::nullopt;
  }
  std::optional<ClusterId> cluster = read_identity_value<ClusterId>(w, name);
  if (!cluster.has_value()) {
    return std::nullopt;
  }
  std::optional<RackAgentBootId> boot = read_identity_value<RackAgentBootId>(w, name);
  if (!boot.has_value()) {
    return std::nullopt;
  }
  std::string role;
  if (!read_string(w, role, name)) {
    return std::nullopt;
  }
  std::string build;
  if (!read_string(w, build, name)) {
    return std::nullopt;
  }
  return HelloMessage{protocol_version, std::move(*cluster), std::move(*boot), std::move(role),
                      std::move(build)};
}

void write_hello_ack(WireWriter& w, const HelloAckMessage& message) {
  w.writer.u16(message.protocol_version);
  write_identity(w, message.cluster);
  write_counter(w, message.coordinator_epoch);
  write_counter(w, message.cluster_epoch);
  write_counter(w, message.topology_epoch);
  write_counter(w, message.topology_generation);
  write_enum(w, message.lifecycle);
  w.writer.boolean(message.accepted);
  write_enum(w, message.reason);
  w.writer.text(message.detail);
}

[[nodiscard]] std::optional<HelloAckMessage> read_hello_ack(WireReader& w, const char* name) {
  std::uint16_t protocol_version = 0;
  if (!read_u16(w, protocol_version, name)) {
    return std::nullopt;
  }
  std::optional<ClusterId> cluster = read_identity_value<ClusterId>(w, name);
  if (!cluster.has_value()) {
    return std::nullopt;
  }
  std::optional<CoordinatorEpoch> coordinator_epoch = read_counter_value<CoordinatorEpoch>(w, name);
  if (!coordinator_epoch.has_value()) {
    return std::nullopt;
  }
  std::optional<ClusterEpoch> cluster_epoch = read_counter_value<ClusterEpoch>(w, name);
  if (!cluster_epoch.has_value()) {
    return std::nullopt;
  }
  std::optional<TopologyEpoch> topology_epoch = read_counter_value<TopologyEpoch>(w, name);
  if (!topology_epoch.has_value()) {
    return std::nullopt;
  }
  std::optional<TopologyGeneration> topology_generation =
      read_counter_value<TopologyGeneration>(w, name);
  if (!topology_generation.has_value()) {
    return std::nullopt;
  }
  ClusterLifecycle lifecycle = ClusterLifecycle::Declared;
  if (!read_enum(w, lifecycle, kMaxClusterLifecycleRaw, name)) {
    return std::nullopt;
  }
  bool accepted = false;
  if (!read_bool(w, accepted, name)) {
    return std::nullopt;
  }
  RejectionReason reason = RejectionReason::None;
  if (!read_enum(w, reason, kMaxRejectionReasonRaw, name)) {
    return std::nullopt;
  }
  std::string detail;
  if (!read_string(w, detail, name)) {
    return std::nullopt;
  }
  return HelloAckMessage{protocol_version, std::move(*cluster), *coordinator_epoch,
                         *cluster_epoch,   *topology_epoch,    *topology_generation,
                         lifecycle,        accepted,            reason,
                         std::move(detail)};
}

void write_register_publisher(WireWriter& w, const RegisterPublisherMessage& message) {
  write_identity(w, message.cluster);
  write_authority(w, message.authority);
  write_identity(w, message.publisher);
  write_identity(w, message.rack);
  write_counter(w, message.generation);
  write_enum(w, message.rack_lifecycle);
  write_composition(w, message.composition);
  w.writer.text(message.origin_label);
  write_timestamp(w, message.sent_at);
}

[[nodiscard]] std::optional<RegisterPublisherMessage> read_register_publisher(WireReader& w,
                                                                             const char* name) {
  std::optional<ClusterId> cluster = read_identity_value<ClusterId>(w, name);
  if (!cluster.has_value()) {
    return std::nullopt;
  }
  std::optional<MutationAuthority> authority = read_authority(w, name);
  if (!authority.has_value()) {
    return std::nullopt;
  }
  std::optional<RackPublisherId> publisher = read_identity_value<RackPublisherId>(w, name);
  if (!publisher.has_value()) {
    return std::nullopt;
  }
  std::optional<RackId> rack = read_identity_value<RackId>(w, name);
  if (!rack.has_value()) {
    return std::nullopt;
  }
  std::optional<RackGeneration> generation = read_counter_value<RackGeneration>(w, name);
  if (!generation.has_value()) {
    return std::nullopt;
  }
  RackLifecycleState rack_lifecycle = RackLifecycleState::Unknown;
  if (!read_enum(w, rack_lifecycle, kMaxRackLifecycleStateRaw, name)) {
    return std::nullopt;
  }
  std::optional<RackCompositionSummary> composition = read_composition(w, name);
  if (!composition.has_value()) {
    return std::nullopt;
  }
  std::string origin_label;
  if (!read_string(w, origin_label, name)) {
    return std::nullopt;
  }
  std::int64_t sent_at = 0;
  if (!read_i64(w, sent_at, name)) {
    return std::nullopt;
  }
  return RegisterPublisherMessage{std::move(*cluster),
                                  std::move(*authority),
                                  std::move(*publisher),
                                  std::move(*rack),
                                  *generation,
                                  rack_lifecycle,
                                  std::move(*composition),
                                  std::move(origin_label),
                                  Timestamp::from_unix_millis(sent_at)};
}

void write_register_ack(WireWriter& w, const RegisterAckMessage& message) {
  w.writer.boolean(message.accepted);
  write_enum(w, message.reason);
  write_counter(w, message.coordinator_epoch);
  write_counter(w, message.cluster_epoch);
  write_counter(w, message.topology_epoch);
  write_counter(w, message.topology_generation);
  write_counter(w, message.membership_generation);
  w.writer.text(message.detail);
}

[[nodiscard]] std::optional<RegisterAckMessage> read_register_ack(WireReader& w, const char* name) {
  RegisterAckMessage message;
  if (!read_bool(w, message.accepted, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, message.reason, kMaxRejectionReasonRaw, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, message.coordinator_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, message.cluster_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, message.topology_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, message.topology_generation, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, message.membership_generation, name)) {
    return std::nullopt;
  }
  if (!read_string(w, message.detail, name)) {
    return std::nullopt;
  }
  return message;
}

void write_heartbeat(WireWriter& w, const HeartbeatMessage& message) {
  write_identity(w, message.cluster);
  write_authority(w, message.authority);
  write_timestamp(w, message.sent_at);
}

[[nodiscard]] std::optional<HeartbeatMessage> read_heartbeat(WireReader& w, const char* name) {
  std::optional<ClusterId> cluster = read_identity_value<ClusterId>(w, name);
  if (!cluster.has_value()) {
    return std::nullopt;
  }
  std::optional<MutationAuthority> authority = read_authority(w, name);
  if (!authority.has_value()) {
    return std::nullopt;
  }
  std::int64_t sent_at = 0;
  if (!read_i64(w, sent_at, name)) {
    return std::nullopt;
  }
  return HeartbeatMessage{std::move(*cluster), std::move(*authority),
                          Timestamp::from_unix_millis(sent_at)};
}

void write_heartbeat_ack(WireWriter& w, const HeartbeatAckMessage& message) {
  write_counter(w, message.coordinator_epoch);
  write_counter(w, message.cluster_epoch);
  write_counter(w, message.topology_epoch);
  write_enum(w, message.lifecycle);
  w.writer.boolean(message.accepted);
  write_enum(w, message.reason);
  w.writer.text(message.detail);
}

[[nodiscard]] std::optional<HeartbeatAckMessage> read_heartbeat_ack(WireReader& w,
                                                                  const char* name) {
  HeartbeatAckMessage message;
  if (!read_counter(w, message.coordinator_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, message.cluster_epoch, name)) {
    return std::nullopt;
  }
  if (!read_counter(w, message.topology_epoch, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, message.lifecycle, kMaxClusterLifecycleRaw, name)) {
    return std::nullopt;
  }
  if (!read_bool(w, message.accepted, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, message.reason, kMaxRejectionReasonRaw, name)) {
    return std::nullopt;
  }
  if (!read_string(w, message.detail, name)) {
    return std::nullopt;
  }
  return message;
}

void write_publish_request(WireWriter& w, const PublishRequestMessage& message) {
  write_mutation_request(w, message.request);
}

[[nodiscard]] std::optional<PublishRequestMessage> read_publish_request(WireReader& w,
                                                                       const char* name) {
  std::optional<MutationRequest> request = read_mutation_request(w, name);
  if (!request.has_value()) {
    return std::nullopt;
  }
  return PublishRequestMessage{std::move(*request)};
}

void write_publish_result(WireWriter& w, const PublishResultMessage& message) {
  write_mutation_result(w, message.result);
}

[[nodiscard]] std::optional<PublishResultMessage> read_publish_result(WireReader& w,
                                                                     const char* name) {
  std::optional<MutationResult> result = read_mutation_result(w, name);
  if (!result.has_value()) {
    return std::nullopt;
  }
  return PublishResultMessage{std::move(*result)};
}

void write_snapshot_request(WireWriter& w, const SnapshotRequestMessage& message) {
  write_identity(w, message.cluster);
}

[[nodiscard]] std::optional<SnapshotRequestMessage> read_snapshot_request(WireReader& w,
                                                                         const char* name) {
  std::optional<ClusterId> cluster = read_identity_value<ClusterId>(w, name);
  if (!cluster.has_value()) {
    return std::nullopt;
  }
  return SnapshotRequestMessage{std::move(*cluster)};
}

void write_snapshot_response(WireWriter& w, const SnapshotResponseMessage& message) {
  w.writer.boolean(message.accepted);
  write_enum(w, message.reason);
  w.writer.text(message.detail);
  write_snapshot_view(w, message.view);
}

[[nodiscard]] std::optional<SnapshotResponseMessage> read_snapshot_response(WireReader& w,
                                                                           const char* name) {
  bool accepted = false;
  if (!read_bool(w, accepted, name)) {
    return std::nullopt;
  }
  RejectionReason reason = RejectionReason::None;
  if (!read_enum(w, reason, kMaxRejectionReasonRaw, name)) {
    return std::nullopt;
  }
  std::string detail;
  if (!read_string(w, detail, name)) {
    return std::nullopt;
  }
  std::optional<SnapshotView> view = read_snapshot_view(w, name);
  if (!view.has_value()) {
    return std::nullopt;
  }
  return SnapshotResponseMessage{accepted, reason, std::move(detail), std::move(*view)};
}

void write_validate_request(WireWriter& w, const ValidateSnapshotRequestMessage& message) {
  write_snapshot_view(w, message.view);
}

[[nodiscard]] std::optional<ValidateSnapshotRequestMessage> read_validate_request(
    WireReader& w, const char* name) {
  std::optional<SnapshotView> view = read_snapshot_view(w, name);
  if (!view.has_value()) {
    return std::nullopt;
  }
  return ValidateSnapshotRequestMessage{std::move(*view)};
}

void write_validate_response(WireWriter& w, const ValidateSnapshotResponseMessage& message) {
  write_snapshot_validation(w, message.validation);
}

[[nodiscard]] std::optional<ValidateSnapshotResponseMessage> read_validate_response(
    WireReader& w, const char* name) {
  std::optional<SnapshotValidation> validation = read_snapshot_validation(w, name);
  if (!validation.has_value()) {
    return std::nullopt;
  }
  return ValidateSnapshotResponseMessage{std::move(*validation)};
}

void write_revalidate_request(WireWriter& w, const RevalidateRequestMessage& message) {
  write_identity(w, message.cluster);
  write_authority(w, message.authority);
  write_identity_vector(w, message.racks, kMaxRacksPerCluster);
}

[[nodiscard]] std::optional<RevalidateRequestMessage> read_revalidate_request(WireReader& w,
                                                                             const char* name) {
  std::optional<ClusterId> cluster = read_identity_value<ClusterId>(w, name);
  if (!cluster.has_value()) {
    return std::nullopt;
  }
  std::optional<MutationAuthority> authority = read_authority(w, name);
  if (!authority.has_value()) {
    return std::nullopt;
  }
  std::vector<RackId> racks;
  if (!read_identity_vector(w, racks, kMaxRacksPerCluster, name)) {
    return std::nullopt;
  }
  return RevalidateRequestMessage{std::move(*cluster), std::move(*authority), std::move(racks)};
}

void write_revalidate_response(WireWriter& w, const RevalidateResponseMessage& message) {
  w.writer.boolean(message.accepted);
  write_enum(w, message.reason);
  w.writer.text(message.detail);
  write_enum(w, message.lifecycle);
  write_readiness_evaluation(w, message.readiness);
}

[[nodiscard]] std::optional<RevalidateResponseMessage> read_revalidate_response(WireReader& w,
                                                                               const char* name) {
  RevalidateResponseMessage message;
  if (!read_bool(w, message.accepted, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, message.reason, kMaxRejectionReasonRaw, name)) {
    return std::nullopt;
  }
  if (!read_string(w, message.detail, name)) {
    return std::nullopt;
  }
  if (!read_enum(w, message.lifecycle, kMaxClusterLifecycleRaw, name)) {
    return std::nullopt;
  }
  std::optional<ReadinessEvaluation> readiness = read_readiness_evaluation(w, name);
  if (!readiness.has_value()) {
    return std::nullopt;
  }
  message.readiness = std::move(*readiness);
  return message;
}

void write_shutdown(WireWriter& w, const ShutdownMessage& message) {
  w.writer.text(message.reason);
}

[[nodiscard]] std::optional<ShutdownMessage> read_shutdown(WireReader& w, const char* name) {
  ShutdownMessage message;
  if (!read_string(w, message.reason, name)) {
    return std::nullopt;
  }
  return message;
}

void write_error_message(WireWriter& w, const ErrorMessage& message) {
  write_structured_error(w, message.error);
}

[[nodiscard]] std::optional<ErrorMessage> read_error_message(WireReader& w, const char* name) {
  std::optional<StructuredError> error = read_structured_error(w, name);
  if (!error.has_value()) {
    return std::nullopt;
  }
  ErrorMessage message;
  message.error = std::move(*error);
  return message;
}


template <class Message, class Write>
[[nodiscard]] std::string encode_message(const Message& message, Write&& write_body) {
  ByteWriter writer;
  WireWriter w{writer};
  write_version_tag(w);
  write_body(w, message);
  return finish_payload(writer, w);
}

template <class Message, class Read>
[[nodiscard]] DecodeOutcome decode_message(std::string_view payload, Message& out,
                                           std::string_view subject, Read&& read_body) {
  ByteReader reader(payload);
  WireReader w{reader};
  if (!read_version_tag(w, "version")) {
    return make_decode_error(w, subject);
  }
  std::optional<Message> message = read_body(w);
  if (!message.has_value()) {
    return make_decode_error(w, subject);
  }
  if (!reader.exhausted()) {
    return trailing_garbage(reader.remaining(), subject);
  }
  out = std::move(*message);
  return DecodeOutcome{};
}

}  // namespace

// ---------------------------------------------------------------------------
// Message payloads
// ---------------------------------------------------------------------------

std::string encode_hello(const HelloMessage& message) {
  return encode_message(message, [](WireWriter& w, const HelloMessage& value) {
    write_hello(w, value);
  });
}

DecodeOutcome decode_hello(std::string_view payload, HelloMessage& out) {
  return decode_message(payload, out, "hello", [](WireReader& w) { return read_hello(w, "hello"); });
}

std::string encode_hello_ack(const HelloAckMessage& message) {
  return encode_message(message, [](WireWriter& w, const HelloAckMessage& value) {
    write_hello_ack(w, value);
  });
}

DecodeOutcome decode_hello_ack(std::string_view payload, HelloAckMessage& out) {
  return decode_message(payload, out, "hello_ack",
                        [](WireReader& w) { return read_hello_ack(w, "hello_ack"); });
}

std::string encode_register_publisher(const RegisterPublisherMessage& message) {
  return encode_message(message, [](WireWriter& w, const RegisterPublisherMessage& value) {
    write_register_publisher(w, value);
  });
}

DecodeOutcome decode_register_publisher(std::string_view payload, RegisterPublisherMessage& out) {
  return decode_message(payload, out, "register_publisher",
                        [](WireReader& w) { return read_register_publisher(w, "register_publisher"); });
}

std::string encode_register_ack(const RegisterAckMessage& message) {
  return encode_message(message, [](WireWriter& w, const RegisterAckMessage& value) {
    write_register_ack(w, value);
  });
}

DecodeOutcome decode_register_ack(std::string_view payload, RegisterAckMessage& out) {
  return decode_message(payload, out, "register_ack",
                        [](WireReader& w) { return read_register_ack(w, "register_ack"); });
}

std::string encode_publish_request(const PublishRequestMessage& message) {
  return encode_message(message, [](WireWriter& w, const PublishRequestMessage& value) {
    write_publish_request(w, value);
  });
}

DecodeOutcome decode_publish_request(std::string_view payload, PublishRequestMessage& out) {
  return decode_message(payload, out, "publish_request",
                        [](WireReader& w) { return read_publish_request(w, "publish_request"); });
}

std::string encode_publish_result(const PublishResultMessage& message) {
  return encode_message(message, [](WireWriter& w, const PublishResultMessage& value) {
    write_publish_result(w, value);
  });
}

DecodeOutcome decode_publish_result(std::string_view payload, PublishResultMessage& out) {
  return decode_message(payload, out, "publish_result",
                        [](WireReader& w) { return read_publish_result(w, "publish_result"); });
}

std::string encode_heartbeat(const HeartbeatMessage& message) {
  return encode_message(message, [](WireWriter& w, const HeartbeatMessage& value) {
    write_heartbeat(w, value);
  });
}

DecodeOutcome decode_heartbeat(std::string_view payload, HeartbeatMessage& out) {
  return decode_message(payload, out, "heartbeat",
                        [](WireReader& w) { return read_heartbeat(w, "heartbeat"); });
}

std::string encode_heartbeat_ack(const HeartbeatAckMessage& message) {
  return encode_message(message, [](WireWriter& w, const HeartbeatAckMessage& value) {
    write_heartbeat_ack(w, value);
  });
}

DecodeOutcome decode_heartbeat_ack(std::string_view payload, HeartbeatAckMessage& out) {
  return decode_message(payload, out, "heartbeat_ack",
                        [](WireReader& w) { return read_heartbeat_ack(w, "heartbeat_ack"); });
}

std::string encode_snapshot_request(const SnapshotRequestMessage& message) {
  return encode_message(message, [](WireWriter& w, const SnapshotRequestMessage& value) {
    write_snapshot_request(w, value);
  });
}

DecodeOutcome decode_snapshot_request(std::string_view payload, SnapshotRequestMessage& out) {
  return decode_message(payload, out, "snapshot_request",
                        [](WireReader& w) { return read_snapshot_request(w, "snapshot_request"); });
}

std::string encode_snapshot_response(const SnapshotResponseMessage& message) {
  return encode_message(message, [](WireWriter& w, const SnapshotResponseMessage& value) {
    write_snapshot_response(w, value);
  });
}

DecodeOutcome decode_snapshot_response(std::string_view payload, SnapshotResponseMessage& out) {
  return decode_message(payload, out, "snapshot_response",
                        [](WireReader& w) { return read_snapshot_response(w, "snapshot_response"); });
}

std::string encode_validate_request(const ValidateSnapshotRequestMessage& message) {
  return encode_message(message, [](WireWriter& w, const ValidateSnapshotRequestMessage& value) {
    write_validate_request(w, value);
  });
}

DecodeOutcome decode_validate_request(std::string_view payload,
                                      ValidateSnapshotRequestMessage& out) {
  return decode_message(
      payload, out, "validate_snapshot_request",
      [](WireReader& w) { return read_validate_request(w, "validate_snapshot_request"); });
}

std::string encode_validate_response(const ValidateSnapshotResponseMessage& message) {
  return encode_message(message, [](WireWriter& w, const ValidateSnapshotResponseMessage& value) {
    write_validate_response(w, value);
  });
}

DecodeOutcome decode_validate_response(std::string_view payload,
                                       ValidateSnapshotResponseMessage& out) {
  return decode_message(
      payload, out, "validate_snapshot_response",
      [](WireReader& w) { return read_validate_response(w, "validate_snapshot_response"); });
}

std::string encode_revalidate_request(const RevalidateRequestMessage& message) {
  return encode_message(message, [](WireWriter& w, const RevalidateRequestMessage& value) {
    write_revalidate_request(w, value);
  });
}

DecodeOutcome decode_revalidate_request(std::string_view payload, RevalidateRequestMessage& out) {
  return decode_message(
      payload, out, "revalidate_request",
      [](WireReader& w) { return read_revalidate_request(w, "revalidate_request"); });
}

std::string encode_revalidate_response(const RevalidateResponseMessage& message) {
  return encode_message(message, [](WireWriter& w, const RevalidateResponseMessage& value) {
    write_revalidate_response(w, value);
  });
}

DecodeOutcome decode_revalidate_response(std::string_view payload,
                                         RevalidateResponseMessage& out) {
  return decode_message(
      payload, out, "revalidate_response",
      [](WireReader& w) { return read_revalidate_response(w, "revalidate_response"); });
}

std::string encode_shutdown(const ShutdownMessage& message) {
  return encode_message(message, [](WireWriter& w, const ShutdownMessage& value) {
    write_shutdown(w, value);
  });
}

DecodeOutcome decode_shutdown(std::string_view payload, ShutdownMessage& out) {
  return decode_message(payload, out, "shutdown",
                        [](WireReader& w) { return read_shutdown(w, "shutdown"); });
}

std::string encode_error(const ErrorMessage& message) {
  return encode_message(message, [](WireWriter& w, const ErrorMessage& value) {
    write_error_message(w, value);
  });
}

DecodeOutcome decode_error(std::string_view payload, ErrorMessage& out) {
  return decode_message(payload, out, "error",
                        [](WireReader& w) { return read_error_message(w, "error"); });
}

// ---------------------------------------------------------------------------
// Shared payload codecs, also used by persistence for durable fields
// ---------------------------------------------------------------------------

bool encode_mutation_request(const MutationRequest& request, ByteWriter& writer) {
  WireWriter w{writer};
  write_version_tag(w);
  write_mutation_request(w, request);
  return w.good();
}

DecodeOutcome decode_mutation_request(ByteReader& reader, MutationRequest& out) {
  WireReader w{reader};
  if (!read_version_tag(w, "mutation_request")) {
    return make_decode_error(w, "mutation_request");
  }
  std::optional<MutationRequest> request = read_mutation_request(w, "mutation_request");
  if (!request.has_value()) {
    return make_decode_error(w, "mutation_request");
  }
  out = std::move(*request);
  return DecodeOutcome{};
}

bool encode_mutation_result(const MutationResult& result, ByteWriter& writer) {
  WireWriter w{writer};
  write_version_tag(w);
  write_mutation_result(w, result);
  return w.good();
}

DecodeOutcome decode_mutation_result(ByteReader& reader, MutationResult& out) {
  WireReader w{reader};
  if (!read_version_tag(w, "mutation_result")) {
    return make_decode_error(w, "mutation_result");
  }
  std::optional<MutationResult> result = read_mutation_result(w, "mutation_result");
  if (!result.has_value()) {
    return make_decode_error(w, "mutation_result");
  }
  out = std::move(*result);
  return DecodeOutcome{};
}

bool encode_snapshot_view(const SnapshotView& view, ByteWriter& writer) {
  WireWriter w{writer};
  write_version_tag(w);
  write_snapshot_view(w, view);
  return w.good();
}

DecodeOutcome decode_snapshot_view(ByteReader& reader, SnapshotView& out) {
  WireReader w{reader};
  if (!read_version_tag(w, "snapshot_view")) {
    return make_decode_error(w, "snapshot_view");
  }
  std::optional<SnapshotView> view = read_snapshot_view(w, "snapshot_view");
  if (!view.has_value()) {
    return make_decode_error(w, "snapshot_view");
  }
  out = std::move(*view);
  return DecodeOutcome{};
}

bool encode_structured_error(const StructuredError& error, ByteWriter& writer) {
  WireWriter w{writer};
  write_version_tag(w);
  write_structured_error(w, error);
  return w.good();
}

DecodeOutcome decode_structured_error(ByteReader& reader, StructuredError& out) {
  WireReader w{reader};
  if (!read_version_tag(w, "structured_error")) {
    return make_decode_error(w, "structured_error");
  }
  std::optional<StructuredError> error = read_structured_error(w, "structured_error");
  if (!error.has_value()) {
    return make_decode_error(w, "structured_error");
  }
  out = std::move(*error);
  return DecodeOutcome{};
}

// ---------------------------------------------------------------------------
// SnapshotView
// ---------------------------------------------------------------------------

SnapshotView SnapshotView::from_snapshot(const ClusterSnapshot& snapshot) {
  if (!snapshot.valid()) {
    // A snapshot that holds no state has nothing to report. The view keeps the
    // UNKNOWN identity and zero counts rather than fabricating a cluster, and
    // the null state is never dereferenced.
    return SnapshotView{};
  }
  const ClusterState& state = snapshot.state();
  const std::size_t domain_count = state.placement_domains.size() + state.capacity_domains.size() +
                                   state.failure_domains.size() + state.network_domains.size() +
                                   state.storage_domains.size() + state.power_domains.size() +
                                   state.cooling_domains.size() + state.link_domains.size();
  return SnapshotView{snapshot.cluster(),
                      snapshot.cluster_epoch(),
                      snapshot.coordinator_epoch(),
                      snapshot.cluster_generation(),
                      snapshot.membership_generation(),
                      snapshot.topology_epoch(),
                      snapshot.topology_generation(),
                      snapshot.connectivity_generation(),
                      snapshot.health_generation(),
                      snapshot.constraint_generation(),
                      snapshot.domain_generations(),
                      snapshot.snapshot_generation(),
                      snapshot.publication_generation(),
                      snapshot.lifecycle(),
                      snapshot.rack_bindings(),
                      snapshot.semantic_digest(),
                      state.racks.size(),
                      state.links.size(),
                      domain_count};
}

}  // namespace cluster_fabric
