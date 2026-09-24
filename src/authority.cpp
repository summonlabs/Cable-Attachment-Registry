// Cable Attachment Registry — authority vocabulary.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/authority.hpp"

#include <array>
#include <utility>

namespace cable_registry {
namespace {

constexpr std::array<std::pair<const char*, AuthorityClass>, 5> kAuthorityNames = {{
    {"Speculative", AuthorityClass::Speculative},
    {"Imported", AuthorityClass::Imported},
    {"Observed", AuthorityClass::Observed},
    {"Declared", AuthorityClass::Declared},
    {"Authoritative", AuthorityClass::Authoritative},
}};

constexpr std::array<std::pair<const char*, ProvenanceClass>, 7> kProvenanceNames = {{
    {"Unknown", ProvenanceClass::Unknown},
    {"OperatorEntry", ProvenanceClass::OperatorEntry},
    {"AssetImport", ProvenanceClass::AssetImport},
    {"DiscoveryAgent", ProvenanceClass::DiscoveryAgent},
    {"PhysicalMeasurement", ProvenanceClass::PhysicalMeasurement},
    {"Reconciliation", ProvenanceClass::Reconciliation},
    {"Synthetic", ProvenanceClass::Synthetic},
}};

} // namespace

const char* to_string(AuthorityClass value) noexcept {
  for (const auto& entry : kAuthorityNames) {
    if (entry.second == value) {
      return entry.first;
    }
  }
  return "Speculative";
}

bool parse_authority_class(std::string_view text, AuthorityClass& out) noexcept {
  for (const auto& entry : kAuthorityNames) {
    if (text == entry.first) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

const char* to_string(ProvenanceClass value) noexcept {
  for (const auto& entry : kProvenanceNames) {
    if (entry.second == value) {
      return entry.first;
    }
  }
  return "Unknown";
}

bool parse_provenance_class(std::string_view text, ProvenanceClass& out) noexcept {
  for (const auto& entry : kProvenanceNames) {
    if (text == entry.first) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

} // namespace cable_registry
