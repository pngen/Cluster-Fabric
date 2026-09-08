// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Parsing and rendering of monotonic generations and epochs.
//
// Rendering is deterministic and allocation free after the first call on a
// thread: each call formats into the next buffer of a per-thread ring of four
// strings. The returned view therefore stays valid until the same thread has
// made four further calls to to_string(ClusterEpoch) or to_string(TopologyEpoch).

#include "cluster_fabric/generation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace cluster_fabric {
namespace {

/// Number of per-thread rendering buffers. A returned view is invalidated only
/// once the same buffer is reused, which takes this many further calls.
constexpr std::size_t kCounterTextBufferCount = 4;

/// Decimal digits of a 64-bit value never exceed this count.
constexpr std::size_t kMaxCounterDigits = 20;

struct CounterTextRing {
  std::array<std::string, kCounterTextBufferCount> buffers{};
  std::size_t next = 0;
};

/// Formats p value into the next buffer of the per-thread ring and returns a
/// view into it. The view is valid until the next kCounterTextBufferCount
/// calls to this function on the same thread.
[[nodiscard]] std::string_view render_counter(std::uint64_t value) noexcept {
  thread_local CounterTextRing ring{};
  std::string& buffer = ring.buffers[ring.next];
  ring.next = (ring.next + 1u) % kCounterTextBufferCount;

  char digits[kMaxCounterDigits];
  std::size_t count = 0;
  do {
    digits[count] = static_cast<char>('0' + static_cast<int>(value % 10u));
    ++count;
    value /= 10u;
  } while (value != 0);

  buffer.clear();
  while (count > 0) {
    --count;
    buffer.push_back(digits[count]);
  }
  return std::string_view(buffer);
}

}  // namespace

std::optional<std::uint64_t> parse_counter_text(std::string_view text, bool allow_zero) noexcept {
  if (text.empty()) {
    return std::nullopt;
  }
  if (text.size() > kMaxCounterDigits) {
    return std::nullopt;
  }

  const std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (kMax - digit) / 10u) {
      return std::nullopt;
    }
    value = value * 10u + digit;
  }

  if (value == 0 && !allow_zero) {
    return std::nullopt;
  }
  return value;
}

std::string_view to_string(ClusterEpoch value) noexcept { return render_counter(value.value()); }

std::string_view to_string(TopologyEpoch value) noexcept { return render_counter(value.value()); }

}  // namespace cluster_fabric
