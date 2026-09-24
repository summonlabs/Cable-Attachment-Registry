// Cable Attachment Registry — physical object and endpoint descriptors.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_IDENTITY_HPP
#define CABLE_REGISTRY_IDENTITY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "cable_registry/capability.hpp"
#include "cable_registry/ids.hpp"

namespace cable_registry {

/// Resting lifecycle state of a physical object.
///
/// "Attached" and "Unattached" are derived from the authoritative attachment
/// graph, never stored directly. A move and a physical replacement are
/// transitions rather than resting states: after a move the object rests in
/// Attached at its new port, and after a replacement it rests in whatever the
/// new incarnation's evidence supports. Both transitions are recorded in the
/// object history with their own event kinds.
enum class LifecycleState : std::uint8_t {
  /// Registered, and no attachment or detachment has ever been asserted for it.
  Registered = 0,
  /// At least one side has an effective detachment assertion and no side has an
  /// authoritative attachment.
  Unattached,
  /// At least one side has an authoritative attachment.
  Attached,
  /// Excluded from the authoritative graph until an equal or higher authority
  /// releases it. Quarantine preserves the underlying claims.
  Quarantined,
  /// Physically removed from the plant. Restorable by a strictly higher
  /// authority, or by a reincarnation.
  Removed,
  /// Terminal. Only a reincarnation can bring the identity back into service.
  Retired,
  /// Sources at the same authority assert incompatible lifecycle states. The
  /// registry reports the conflict instead of picking one.
  Conflicting,
};

CABLE_REGISTRY_API const char* to_string(LifecycleState value) noexcept;
CABLE_REGISTRY_API bool parse_lifecycle_state(std::string_view text, LifecycleState& out) noexcept;

/// The lifecycle value one source asserts. A source asserts the state it
/// believes the object is in; the registry folds the assertions.
enum class LifecycleAction : std::uint8_t {
  /// The object is present and in service.
  Present = 0,
  Quarantined,
  Removed,
  Retired,
};

CABLE_REGISTRY_API const char* to_string(LifecycleAction value) noexcept;
CABLE_REGISTRY_API bool parse_lifecycle_action(std::string_view text, LifecycleAction& out) noexcept;

/// Kind of lifecycle transition recorded in an object's history.
enum class LifecycleEventKind : std::uint8_t {
  Registered = 0,
  Attached,
  Detached,
  Moved,
  Reincarnated,
  Quarantined,
  Released,
  Removed,
  Retired,
};

CABLE_REGISTRY_API const char* to_string(LifecycleEventKind value) noexcept;

/// One connector position of a physical object.
///
/// A side exists even when it declares nothing: an object with two sides and
/// no declarations is the normal case for a plain patch cord, and the sides
/// are then described by the object level NominalCapability.
struct ObjectSideDescriptor {
  /// Connector at this position. Unknown means not declared.
  ConnectorClass connector = ConnectorClass::Unknown;
  /// Media from this position onwards. Unknown means not declared.
  MediaClass media = MediaClass::Unknown;
  /// Lanes carried by this position. Zero means not declared. On a breakout
  /// harness this is what distinguishes a 12 fibre trunk side from a duplex
  /// fan-out side.
  LaneCount lanes{};

  friend bool operator==(const ObjectSideDescriptor&, const ObjectSideDescriptor&) noexcept = default;
};

/// A registered physical object.
///
/// The printed label and the serial-like string are attributes with reuse
/// detection, not identity. Identity is id. Reusing a label of a live object
/// is refused, which is what stops evidence recorded against a replaced unit
/// from binding to its successor.
struct ObjectDescriptor {
  ObjectId id{};
  ObjectKind kind = ObjectKind::Unknown;
  /// Human readable label printed on the object, for example "C-17".
  std::string physical_label;
  /// Serial-like string, for example a vendor serial or an asset tag.
  std::string serial_like;
  /// Administrative location, for example "R04-U12-P3".
  std::string administrative_location;
  /// One entry per connector position, in side order. Must be non-empty and
  /// at most Limits::max_sides_per_object entries long.
  std::vector<ObjectSideDescriptor> sides;
  /// Object level nominal capability. A side descriptor refines it.
  NominalCapability capability;

  friend bool operator==(const ObjectDescriptor&, const ObjectDescriptor&) noexcept = default;
};

/// A registered endpoint. Ports are addressed as PortRef{id, index} and index
/// is always strictly less than port_count.
struct EndpointDescriptor {
  EndpointId id{};
  EndpointKind kind = EndpointKind::Unknown;
  std::string name;
  std::string administrative_location;
  /// Number of ports the endpoint exposes. Must be at least one.
  std::uint32_t port_count = 0;
  /// Capability applied to every port that has no published override.
  PortCapability default_port_capability;

  friend bool operator==(const EndpointDescriptor&, const EndpointDescriptor&) noexcept = default;
};

} // namespace cable_registry

#endif // CABLE_REGISTRY_IDENTITY_HPP
