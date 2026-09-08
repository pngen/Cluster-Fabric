// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Compiler and architecture build identity.
//
// The identity is a pure function of the toolchain and the target pointer
// width. It deliberately contains no date, host name, user name, path or build
// directory, so two builds of the same sources by the same toolchain produce
// the same identity and a report can never leak a build host.

// version.hpp declares std::string version_banner() without including <string>;
// the header is frozen, so this translation unit supplies that dependency
// before including it.
#include <string>

#include "cluster_fabric/version.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cluster_fabric {
namespace {

[[nodiscard]] constexpr std::string_view compiler_family() noexcept {
#if defined(_MSC_VER)
  return "msvc";
#elif defined(__clang__)
  return "clang";
#elif defined(__GNUC__)
  return "gcc";
#else
  return "unknown";
#endif
}

[[nodiscard]] std::string compiler_version_text() {
#if defined(_MSC_VER)
  constexpr int kMajor = _MSC_VER / 100;
  constexpr int kMinor = _MSC_VER % 100;
  return std::to_string(kMajor) + "." + std::to_string(kMinor);
#elif defined(__clang__)
  return std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__);
#elif defined(__GNUC__)
  return std::to_string(__GNUC__);
#else
  return std::string("0");
#endif
}

[[nodiscard]] constexpr std::string_view architecture_name() noexcept {
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_ARM) || defined(__arm__)
  return "arm32";
#elif defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
  return "x64";
#else
  return "x86_64";
#endif
#elif defined(_M_IX86) || defined(__i386__)
  return "x86";
#else
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFull
  return "64";
#else
  return "32";
#endif
#endif
}

}  // namespace

std::string_view build_identity() noexcept {
  static const std::string identity = [] {
    std::string out;
    out.reserve(32);
    out.append(compiler_family());
    out.push_back('-');
    out.append(compiler_version_text());
    out.push_back('/');
    out.append(architecture_name());
    return out;
  }();
  return identity;
}

std::string version_banner() {
  std::string banner;
  banner.reserve(kProductName.size() + kVersionString.size() + build_identity().size() + 4u);
  banner.append(kProductName);
  banner.push_back(' ');
  banner.append(kVersionString);
  banner.append(" (");
  banner.append(build_identity());
  banner.push_back(')');
  return banner;
}

}  // namespace cluster_fabric
