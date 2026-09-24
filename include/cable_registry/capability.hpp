// Cable Attachment Registry — media, connector and nominal capability metadata.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_CAPABILITY_HPP
#define CABLE_REGISTRY_CAPABILITY_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "cable_registry/export.hpp"

namespace cable_registry {

/// Transport medium a physical object or port is built for.
///
/// This is a declaration carried by the object, not a measurement. The
/// registry never infers media from observed traffic and never claims to have
/// measured one.
enum class MediaClass : std::uint8_t {
  Unknown = 0,
  CopperTwinax,
  CopperTwistedPair,
  MultimodeFiber,
  SinglemodeFiber,
  ActiveOptical,
  DirectAttachCopper,
  PowerBus,
  Other,
};

/// Connector family of a physical object side or port.
///
/// The names are vendor neutral connector form factors, not vendor SKUs. A
/// registry deployment that needs a form factor that is not listed uses
/// Other plus a capability code; it must not reuse an unrelated entry.
enum class ConnectorClass : std::uint8_t {
  Unknown = 0,
  Rj45,
  Sfp,
  SfpPlus,
  Sfp28,
  Qsfp,
  Qsfp28,
  Qsfp56,
  QsfpDd,
  Ospf, // Octal small form factor pluggable.
  Mpo12,
  Mpo16,
  Mpo24,
  Lc,
  Sc,
  Cs,
  Sn,
  BareFiber,
  Other,
};

/// Kind of physical object the registry tracks.
enum class ObjectKind : std::uint8_t {
  Unknown = 0,
  Cable,
  PatchCord,
  Trunk,
  Breakout,
  Adapter,
  PassiveModule,
  Loopback,
  Other,
};

/// Kind of endpoint that owns ports.
enum class EndpointKind : std::uint8_t {
  Unknown = 0,
  Host,
  Switch,
  Router,
  PatchPanel,
  AdapterChassis,
  OpticalShelter,
  Other,
};

/// Number of independent lanes (pairs, fibers, channels) the object carries.
/// Zero means "not declared", which is not the same as one.
struct LaneCount {
  std::uint16_t value = 0;
  friend constexpr bool operator==(const LaneCount&, const LaneCount&) noexcept = default;
  friend constexpr auto operator<=>(const LaneCount&, const LaneCount&) noexcept = default;
};

CABLE_REGISTRY_API const char* to_string(MediaClass value) noexcept;
CABLE_REGISTRY_API const char* to_string(ConnectorClass value) noexcept;
CABLE_REGISTRY_API const char* to_string(ObjectKind value) noexcept;
CABLE_REGISTRY_API const char* to_string(EndpointKind value) noexcept;

/// Parses the exact string returned by the matching to_string overload.
CABLE_REGISTRY_API bool parse_media_class(std::string_view text, MediaClass& out) noexcept;
CABLE_REGISTRY_API bool parse_connector_class(std::string_view text, ConnectorClass& out) noexcept;
CABLE_REGISTRY_API bool parse_object_kind(std::string_view text, ObjectKind& out) noexcept;
CABLE_REGISTRY_API bool parse_endpoint_kind(std::string_view text, EndpointKind& out) noexcept;

/// What a physical object is nominally built for.
///
/// Nominal means "as declared by the manufacturer or the asset record". It is
/// not a measurement and the registry never promotes it to operational state.
struct NominalCapability {
  MediaClass media = MediaClass::Unknown;
  ConnectorClass connector = ConnectorClass::Unknown;
  /// Lanes the object carries. Zero means not declared.
  LaneCount lanes{};
  /// Nominal signalling rate of one lane in kilobits per second. Zero means
  /// not declared.
  std::uint64_t nominal_lane_rate_kbps = 0;
  /// Nominal aggregate rate in kilobits per second. Zero means not declared.
  std::uint64_t nominal_total_kbps = 0;
  /// Nominal reach in millimetres. Zero with length_declared false means not
  /// declared; a declared zero length is not representable.
  std::uint64_t nominal_reach_mm = 0;
  /// True when nominal_reach_mm is a declaration rather than a default.
  bool reach_declared = false;
  /// Vendor neutral capability code such as "400GBASE-CR4".
  std::string capability_code;

  friend bool operator==(const NominalCapability&, const NominalCapability&) noexcept = default;
};

/// What a port accepts and how many attachments it admits at once.
///
/// max_simultaneous_attachments is the only mechanism that lets more than one
/// mutually exclusive attachment claim coexist on one port. When it is one,
/// two distinct subjects on the same port at the same authority are a
/// preserved conflict, never a silent pick.
struct PortCapability {
  std::uint32_t max_simultaneous_attachments = 1;
  /// Connector the port accepts. Unknown means "not declared", which the
  /// compatibility check treats as compatible.
  ConnectorClass accepted_connector = ConnectorClass::Unknown;
  /// Media the port accepts. Unknown means "not declared".
  MediaClass accepted_media = MediaClass::Unknown;
  /// Lanes the port provides. Zero means not declared.
  LaneCount lane_capacity{};
  friend bool operator==(const PortCapability&, const PortCapability&) noexcept = default;
};

} // namespace cable_registry

#endif // CABLE_REGISTRY_CAPABILITY_HPP
