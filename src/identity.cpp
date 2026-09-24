// Cable Attachment Registry — lifecycle vocabulary.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/identity.hpp"

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

constexpr std::array<std::pair<const char*, LifecycleState>, 7> kLifecycleStates = {{
    {"Registered", LifecycleState::Registered},
    {"Unattached", LifecycleState::Unattached},
    {"Attached", LifecycleState::Attached},
    {"Quarantined", LifecycleState::Quarantined},
    {"Removed", LifecycleState::Removed},
    {"Retired", LifecycleState::Retired},
    {"Conflicting", LifecycleState::Conflicting},
}};

constexpr std::array<std::pair<const char*, LifecycleAction>, 4> kLifecycleActions = {{
    {"Present", LifecycleAction::Present},
    {"Quarantined", LifecycleAction::Quarantined},
    {"Removed", LifecycleAction::Removed},
    {"Retired", LifecycleAction::Retired},
}};

constexpr std::array<std::pair<const char*, LifecycleEventKind>, 9> kLifecycleEvents = {{
    {"Registered", LifecycleEventKind::Registered},
    {"Attached", LifecycleEventKind::Attached},
    {"Detached", LifecycleEventKind::Detached},
    {"Moved", LifecycleEventKind::Moved},
    {"Reincarnated", LifecycleEventKind::Reincarnated},
    {"Quarantined", LifecycleEventKind::Quarantined},
    {"Released", LifecycleEventKind::Released},
    {"Removed", LifecycleEventKind::Removed},
    {"Retired", LifecycleEventKind::Retired},
}};

} // namespace

const char* to_string(LifecycleState value) noexcept {
  return lookup(kLifecycleStates, value);
}

bool parse_lifecycle_state(std::string_view text, LifecycleState& out) noexcept {
  for (const auto& entry : kLifecycleStates) {
    if (text == entry.first) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

const char* to_string(LifecycleAction value) noexcept {
  return lookup(kLifecycleActions, value);
}

bool parse_lifecycle_action(std::string_view text, LifecycleAction& out) noexcept {
  for (const auto& entry : kLifecycleActions) {
    if (text == entry.first) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

const char* to_string(LifecycleEventKind value) noexcept {
  return lookup(kLifecycleEvents, value);
}

} // namespace cable_registry
