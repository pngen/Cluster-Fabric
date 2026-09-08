// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Opaque process-incarnation identity generation.
//
// A boot identity is opaque: callers may compare and persist it but must never
// parse meaning out of it. It carries the process id, a monotonic per-process
// sequence and a high-resolution-clock-derived mix, so two incarnations of the
// same process id remain distinguishable, and it is always a validated,
// bounded identity.

#include "cluster_fabric/identity.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace cluster_fabric {
namespace {

/// Used whenever the caller-supplied prefix is not a valid identity.
constexpr std::string_view kFallbackPrefix = "boot";

/// Last-resort text. It is a valid identity by inspection, so parsing it can
/// never fail.
constexpr std::string_view kLastResortBootText = "boot-0-0-0";

/// Per-process monotonic component. The first value handed out is 1.
std::atomic<std::uint64_t> g_boot_sequence{0};

[[nodiscard]] std::uint64_t process_identity() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(static_cast<unsigned int>(::_getpid()));
#else
  return static_cast<std::uint64_t>(static_cast<unsigned long>(::getpid()));
#endif
}

/// Mixes the high-resolution clock into a value with no useful structure for
/// a caller to depend on. Determinism is deliberately not required here: the
/// value only has to distinguish incarnations, and identity text is never used
/// as a deterministic seed.
[[nodiscard]] std::uint64_t clock_mix() noexcept {
  const std::uint64_t ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::uint64_t mixed = ticks + 0x9E3779B97F4A7C15ull;
  mixed ^= mixed >> 30u;
  mixed *= 0xBF58476D1CE4E5B9ull;
  mixed ^= mixed >> 27u;
  mixed *= 0x94D049BB133111EBull;
  mixed ^= mixed >> 31u;
  return mixed;
}

[[nodiscard]] std::size_t decimal_digits(std::uint64_t value) noexcept {
  std::size_t count = 1;
  while (value >= 10u) {
    value /= 10u;
    ++count;
  }
  return count;
}

[[nodiscard]] std::size_t hex_digits(std::uint64_t value) noexcept {
  std::size_t count = 1;
  while (value >= 16u) {
    value >>= 4u;
    ++count;
  }
  return count;
}

void append_decimal(std::string& out, std::uint64_t value) {
  char digits[20];
  std::size_t count = 0;
  do {
    digits[count] = static_cast<char>('0' + static_cast<int>(value % 10u));
    ++count;
    value /= 10u;
  } while (value != 0);
  while (count > 0) {
    --count;
    out.push_back(digits[count]);
  }
}

void append_hex(std::string& out, std::uint64_t value) {
  constexpr char kHexDigits[] = "0123456789abcdef";
  char digits[16];
  std::size_t count = 0;
  do {
    digits[count] = kHexDigits[static_cast<std::size_t>(value & 0xFu)];
    ++count;
    value >>= 4u;
  } while (value != 0);
  while (count > 0) {
    --count;
    out.push_back(digits[count]);
  }
}

/// Number of bytes appended after the prefix: "-<pid>-<sequence>-<hex>".
[[nodiscard]] std::size_t suffix_length(std::uint64_t pid, std::uint64_t sequence,
                                        std::uint64_t mix) noexcept {
  return 3u + decimal_digits(pid) + decimal_digits(sequence) + hex_digits(mix);
}

/// Composes "<prefix>-<pid>-<sequence>-<hex>", truncating the prefix when the
/// combined text would exceed the validated identity bound. The numeric suffix
/// contains only digits, hex letters and '-' separators, so truncation can
/// never introduce a reserved ".." sequence.
[[nodiscard]] std::string compose(std::string_view prefix, std::uint64_t pid,
                                  std::uint64_t sequence, std::uint64_t mix) {
  const std::size_t suffix = suffix_length(pid, sequence, mix);
  std::size_t keep = prefix.size();
  if (keep + suffix > kMaxIdentityLength) {
    keep = suffix < kMaxIdentityLength ? kMaxIdentityLength - suffix : 0u;
  }

  std::string out;
  out.reserve(keep + suffix);
  out.append(prefix.substr(0, keep));
  out.push_back('-');
  append_decimal(out, pid);
  out.push_back('-');
  append_decimal(out, sequence);
  out.push_back('-');
  append_hex(out, mix);
  return out;
}

}  // namespace

std::string_view to_string(IdentityStatus value) noexcept {
  switch (value) {
    case IdentityStatus::Ok:
      return "OK";
    case IdentityStatus::Empty:
      return "EMPTY";
    case IdentityStatus::TooLong:
      return "TOO_LONG";
    case IdentityStatus::InvalidCharacter:
      return "INVALID_CHARACTER";
    case IdentityStatus::Reserved:
      return "RESERVED";
  }
  return "UNKNOWN";
}

std::string make_boot_id_text(std::string_view prefix) {
  const std::string_view safe_prefix =
      validate_identity(prefix).ok() ? prefix : std::string_view(kFallbackPrefix);

  const std::uint64_t pid = process_identity();
  const std::uint64_t sequence = g_boot_sequence.fetch_add(1u, std::memory_order_relaxed) + 1u;
  const std::uint64_t mix = clock_mix();

  std::string text = compose(safe_prefix, pid, sequence, mix);
  if (!validate_identity(text).ok()) {
    // Unreachable for the current bounds; kept so the function stays total if
    // the contract ever changes.
    text = compose(std::string_view{}, pid, sequence, mix);
  }
  return text;
}

RackAgentBootId make_rack_agent_boot_id(std::string_view prefix) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    const std::string_view effective = attempt == 0 ? prefix : std::string_view(kFallbackPrefix);
    const std::string text = make_boot_id_text(effective);
    if (std::optional<RackAgentBootId> parsed = RackAgentBootId::parse(text)) {
      return *parsed;
    }
  }
  // make_boot_id_text always yields a validated identity, so this parse
  // succeeds; the fallback exists only to keep the function total.
  std::optional<RackAgentBootId> last_resort = RackAgentBootId::parse(kLastResortBootText);
  return *last_resort;
}

}  // namespace cluster_fabric
