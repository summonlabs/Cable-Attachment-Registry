// Cable Attachment Registry — strongly typed identities and small ordinals.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_IDS_HPP
#define CABLE_REGISTRY_IDS_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "cable_registry/export.hpp"

namespace cable_registry {

/// A 128 bit opaque identifier.
///
/// A nil value (both halves zero) is never a valid registered identity; every
/// registration path rejects it. The representation is deliberately not a
/// UUID type: the registry only requires a stable, comparable, collision
/// resistant token, and a publisher may derive it from any naming authority
/// it likes.
struct Id128 {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;

  friend constexpr bool operator==(const Id128&, const Id128&) noexcept = default;
  friend constexpr auto operator<=>(const Id128&, const Id128&) noexcept = default;

  [[nodiscard]] constexpr bool is_nil() const noexcept { return hi == 0 && lo == 0; }
};

/// Lowercase 32 character hexadecimal form.
CABLE_REGISTRY_API std::string to_hex(const Id128& value);

/// Parses 32 hexadecimal characters, with optional '-' separators in the
/// canonical 8-4-4-4-12 positions. Returns false when the text is not a
/// well formed identifier and leaves @p out untouched.
CABLE_REGISTRY_API bool parse_hex_id(std::string_view text, Id128& out) noexcept;

namespace detail {
/// Generates a 128 bit identifier from the process entropy source. Not part of
/// the registry contract: identity is always supplied by the caller.
CABLE_REGISTRY_API Id128 random_id128() noexcept;
} // namespace detail

/// Strongly typed view over an Id128. Distinct tags are distinct types, so an
/// ObjectId can never be passed where an EndpointId is expected.
template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Id128 value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongId from_parts(std::uint64_t hi, std::uint64_t lo) noexcept {
    return StrongId(Id128{hi, lo});
  }

  [[nodiscard]] constexpr const Id128& value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_.is_nil(); }

  /// Lowercase hexadecimal form.
  [[nodiscard]] std::string to_hex() const { return cable_registry::to_hex(value_); }

  friend constexpr bool operator==(const StrongId&, const StrongId&) noexcept = default;
  friend constexpr auto operator<=>(const StrongId&, const StrongId&) noexcept = default;

 private:
  Id128 value_{};
};

struct ObjectIdTag {};
struct EndpointIdTag {};
struct SourceIdTag {};
struct EvidenceIdTag {};
struct RegistryIdTag {};

/// Identity of a physical object: a cable, a patch cord, an adapter, a
/// breakout harness, a passive module. Identity is the durable token, never
/// the printed label.
using ObjectId = StrongId<ObjectIdTag>;
/// Identity of a managed endpoint: a host, a switch, a patch panel, an adapter
/// chassis. Ports only exist inside an endpoint.
using EndpointId = StrongId<EndpointIdTag>;
/// Identity of an evidence source: a discovery agent, a DCIM import, an
/// operator runbook, a synthetic generator.
using SourceId = StrongId<SourceIdTag>;
/// Identity of one evidence record, unique across the whole catalogue of a
/// source.
using EvidenceId = StrongId<EvidenceIdTag>;
/// Identity of a registry instance. Minted at first open and persisted.
using RegistryId = StrongId<RegistryIdTag>;

/// Zero based index of a port inside its endpoint.
struct PortIndex {
  std::uint32_t value = 0;
  friend constexpr bool operator==(const PortIndex&, const PortIndex&) noexcept = default;
  friend constexpr auto operator<=>(const PortIndex&, const PortIndex&) noexcept = default;
};

/// Zero based index of a connector position ("side") of a physical object. A
/// two ended cable has sides 0 and 1.
struct SideIndex {
  std::uint32_t value = 0;
  friend constexpr bool operator==(const SideIndex&, const SideIndex&) noexcept = default;
  friend constexpr auto operator<=>(const SideIndex&, const SideIndex&) noexcept = default;
};

/// A port of an endpoint. A port never exists on its own: it is always
/// qualified by the endpoint that owns it.
struct PortRef {
  EndpointId endpoint{};
  PortIndex index{};
  friend constexpr bool operator==(const PortRef&, const PortRef&) noexcept = default;
  friend constexpr auto operator<=>(const PortRef&, const PortRef&) noexcept = default;
};

/// One connector position of one physical object.
struct ObjectSideRef {
  ObjectId object{};
  SideIndex side{};
  friend constexpr bool operator==(const ObjectSideRef&, const ObjectSideRef&) noexcept = default;
  friend constexpr auto operator<=>(const ObjectSideRef&, const ObjectSideRef&) noexcept = default;
};

/// Monotonic incarnation counter of an evidence source. A source that restarts
/// must present a strictly higher incarnation; the registry fences everything
/// recorded under the lower one.
struct Incarnation {
  std::uint64_t value = 0;
  friend constexpr bool operator==(const Incarnation&, const Incarnation&) noexcept = default;
  friend constexpr auto operator<=>(const Incarnation&, const Incarnation&) noexcept = default;
};

/// Monotonic evidence generation inside one source incarnation. The registry
/// orders one source's claims by generation, never by arrival time.
struct Generation {
  std::uint64_t value = 0;
  friend constexpr bool operator==(const Generation&, const Generation&) noexcept = default;
  friend constexpr auto operator<=>(const Generation&, const Generation&) noexcept = default;
};

/// Incarnation counter of a physical object. A physical replacement that keeps
/// the same asset identity advances it; every claim recorded against the old
/// incarnation is fenced and can never bind to the new one.
struct ObjectIncarnation {
  std::uint64_t value = 0;
  friend constexpr bool operator==(const ObjectIncarnation&, const ObjectIncarnation&) noexcept = default;
  friend constexpr auto operator<=>(const ObjectIncarnation&, const ObjectIncarnation&) noexcept = default;
};

/// A source incarnation pair. This is the unit the registry fences and the
/// unit that owns a monotonic generation stream.
struct SourceIncarnationKey {
  SourceId source{};
  Incarnation incarnation{};
  friend constexpr bool operator==(const SourceIncarnationKey&, const SourceIncarnationKey&) noexcept = default;
  friend constexpr auto operator<=>(const SourceIncarnationKey&, const SourceIncarnationKey&) noexcept = default;
};

/// Zero based slot of one attachment inside a port that declares a
/// multi-attachment capability. Always zero on an exclusive port.
struct AttachmentSlot {
  std::uint32_t value = 0;
  friend constexpr bool operator==(const AttachmentSlot&, const AttachmentSlot&) noexcept = default;
  friend constexpr auto operator<=>(const AttachmentSlot&, const AttachmentSlot&) noexcept = default;
};

} // namespace cable_registry

#endif // CABLE_REGISTRY_IDS_HPP
