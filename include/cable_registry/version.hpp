// Cable Attachment Registry — version and build identity.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_VERSION_HPP
#define CABLE_REGISTRY_VERSION_HPP

#include <cstdint>
#include <string>

#include "cable_registry/export.hpp"

namespace cable_registry {

/// Major version of the public API. Changes only on a breaking interface change.
inline constexpr std::uint32_t kVersionMajor = 1;
/// Minor version of the public API. Changes on a backwards compatible addition.
inline constexpr std::uint32_t kVersionMinor = 0;
/// Patch version.
inline constexpr std::uint32_t kVersionPatch = 0;

/// Version of the on-disk store format. A store written by a different major
/// format version is refused rather than guessed at.
inline constexpr std::uint32_t kStoreFormatVersion = 1;

/// Version of the evidence schema carried in every evidence header.
inline constexpr std::uint32_t kEvidenceSchemaVersion = 1;

/// Version of the canonical snapshot encoding. The canonical bytes of a
/// snapshot are only comparable within one encoding version.
inline constexpr std::uint32_t kSnapshotFormatVersion = 1;

/// Version of the registry-to-publisher transport protocol.
inline constexpr std::uint32_t kProtocolVersion = 1;

/// The version as "major.minor.patch".
CABLE_REGISTRY_API std::string version_string();

/// The version as a single integer, major * 10000 + minor * 100 + patch.
CABLE_REGISTRY_API std::uint32_t version_number() noexcept;

} // namespace cable_registry

#endif // CABLE_REGISTRY_VERSION_HPP
