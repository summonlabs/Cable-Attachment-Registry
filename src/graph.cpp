// Cable Attachment Registry — graph vocabulary.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/graph.hpp"

#include <array>
#include <utility>

namespace cable_registry {
namespace {

template <class Enum, std::size_t Size>
const char* lookup(const std::array<std::pair<const char*, Enum>, Size>& table, Enum value) noexcept {
  for (const auto& entry : table) {
    if (entry.second == value) {
      return entry.first;
    }
  }
  return "Unknown";
}

constexpr std::array<std::pair<const char*, ValidationState>, 3> kValidationStates = {{
    {"Unvalidated", ValidationState::Unvalidated},
    {"PartiallyValidated", ValidationState::PartiallyValidated},
    {"Validated", ValidationState::Validated},
}};

constexpr std::array<std::pair<const char*, ValidationPolicy>, 2> kValidationPolicies = {{
    {"IncludeAll", ValidationPolicy::IncludeAll},
    {"LiveOnly", ValidationPolicy::LiveOnly},
}};

constexpr std::array<std::pair<const char*, ConflictReason>, 7> kConflictReasons = {{
    {"None", ConflictReason::None},
    {"TooManyAttachments", ConflictReason::TooManyAttachments},
    {"EmptyAndOccupied", ConflictReason::EmptyAndOccupied},
    {"ObjectSideDoubleBooked", ConflictReason::ObjectSideDoubleBooked},
    {"LifecycleConflict", ConflictReason::LifecycleConflict},
    {"CapabilityConflict", ConflictReason::CapabilityConflict},
    {"IncompatibleConnector", ConflictReason::IncompatibleConnector},
}};

constexpr std::array<std::pair<const char*, ConflictReason>, 1> kMediaReason = {{
    {"IncompatibleMedia", ConflictReason::IncompatibleMedia},
}};

constexpr std::array<std::pair<const char*, PortAttachmentState>, 4> kPortStates = {{
    {"Unknown", PortAttachmentState::Unknown},
    {"Empty", PortAttachmentState::Empty},
    {"Attached", PortAttachmentState::Attached},
    {"Conflicting", PortAttachmentState::Conflicting},
}};

} // namespace

const char* to_string(ValidationState value) noexcept {
  return lookup(kValidationStates, value);
}

const char* to_string(ValidationPolicy value) noexcept {
  return lookup(kValidationPolicies, value);
}

const char* to_string(ConflictReason value) noexcept {
  if (value == ConflictReason::IncompatibleMedia) {
    return lookup(kMediaReason, value);
  }
  return lookup(kConflictReasons, value);
}

const char* to_string(PortAttachmentState value) noexcept {
  return lookup(kPortStates, value);
}

} // namespace cable_registry
