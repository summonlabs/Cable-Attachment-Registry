// Cable Attachment Registry — capability vocabulary.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/capability.hpp"

#include <array>
#include <utility>

namespace cable_registry {
namespace {

template <class Enum, std::size_t Size>
bool parse_from_table(const std::array<std::pair<const char*, Enum>, Size>& table,
                      std::string_view text,
                      Enum& out) noexcept {
  for (const auto& entry : table) {
    if (text == entry.first) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

template <class Enum, std::size_t Size>
const char* name_from_table(const std::array<std::pair<const char*, Enum>, Size>& table, Enum value) noexcept {
  for (const auto& entry : table) {
    if (entry.second == value) {
      return entry.first;
    }
  }
  return "Unknown";
}

constexpr std::array<std::pair<const char*, MediaClass>, 9> kMediaNames = {{
    {"Unknown", MediaClass::Unknown},
    {"CopperTwinax", MediaClass::CopperTwinax},
    {"CopperTwistedPair", MediaClass::CopperTwistedPair},
    {"MultimodeFiber", MediaClass::MultimodeFiber},
    {"SinglemodeFiber", MediaClass::SinglemodeFiber},
    {"ActiveOptical", MediaClass::ActiveOptical},
    {"DirectAttachCopper", MediaClass::DirectAttachCopper},
    {"PowerBus", MediaClass::PowerBus},
    {"Other", MediaClass::Other},
}};

constexpr std::array<std::pair<const char*, ConnectorClass>, 19> kConnectorNames = {{
    {"Unknown", ConnectorClass::Unknown},
    {"Rj45", ConnectorClass::Rj45},
    {"Sfp", ConnectorClass::Sfp},
    {"SfpPlus", ConnectorClass::SfpPlus},
    {"Sfp28", ConnectorClass::Sfp28},
    {"Qsfp", ConnectorClass::Qsfp},
    {"Qsfp28", ConnectorClass::Qsfp28},
    {"Qsfp56", ConnectorClass::Qsfp56},
    {"QsfpDd", ConnectorClass::QsfpDd},
    {"Ospf", ConnectorClass::Ospf},
    {"Mpo12", ConnectorClass::Mpo12},
    {"Mpo16", ConnectorClass::Mpo16},
    {"Mpo24", ConnectorClass::Mpo24},
    {"Lc", ConnectorClass::Lc},
    {"Sc", ConnectorClass::Sc},
    {"Cs", ConnectorClass::Cs},
    {"Sn", ConnectorClass::Sn},
    {"BareFiber", ConnectorClass::BareFiber},
    {"Other", ConnectorClass::Other},
}};

constexpr std::array<std::pair<const char*, ObjectKind>, 9> kObjectKindNames = {{
    {"Unknown", ObjectKind::Unknown},
    {"Cable", ObjectKind::Cable},
    {"PatchCord", ObjectKind::PatchCord},
    {"Trunk", ObjectKind::Trunk},
    {"Breakout", ObjectKind::Breakout},
    {"Adapter", ObjectKind::Adapter},
    {"PassiveModule", ObjectKind::PassiveModule},
    {"Loopback", ObjectKind::Loopback},
    {"Other", ObjectKind::Other},
}};

constexpr std::array<std::pair<const char*, EndpointKind>, 8> kEndpointKindNames = {{
    {"Unknown", EndpointKind::Unknown},
    {"Host", EndpointKind::Host},
    {"Switch", EndpointKind::Switch},
    {"Router", EndpointKind::Router},
    {"PatchPanel", EndpointKind::PatchPanel},
    {"AdapterChassis", EndpointKind::AdapterChassis},
    {"OpticalShelter", EndpointKind::OpticalShelter},
    {"Other", EndpointKind::Other},
}};

} // namespace

const char* to_string(MediaClass value) noexcept {
  return name_from_table(kMediaNames, value);
}

const char* to_string(ConnectorClass value) noexcept {
  return name_from_table(kConnectorNames, value);
}

const char* to_string(ObjectKind value) noexcept {
  return name_from_table(kObjectKindNames, value);
}

const char* to_string(EndpointKind value) noexcept {
  return name_from_table(kEndpointKindNames, value);
}

bool parse_media_class(std::string_view text, MediaClass& out) noexcept {
  return parse_from_table(kMediaNames, text, out);
}

bool parse_connector_class(std::string_view text, ConnectorClass& out) noexcept {
  return parse_from_table(kConnectorNames, text, out);
}

bool parse_object_kind(std::string_view text, ObjectKind& out) noexcept {
  return parse_from_table(kObjectKindNames, text, out);
}

bool parse_endpoint_kind(std::string_view text, EndpointKind& out) noexcept {
  return parse_from_table(kEndpointKindNames, text, out);
}

} // namespace cable_registry
