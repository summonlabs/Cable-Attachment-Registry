// Cable Attachment Registry — evidence vocabulary and typed outcomes.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/evidence.hpp"

#include <array>
#include <type_traits>
#include <utility>

#include "cable_registry/ids.hpp"

namespace cable_registry {
namespace {

constexpr std::array<std::pair<const char*, EvidenceKind>, 9> kEvidenceKinds = {{
    {"RegisterObject", EvidenceKind::RegisterObject},
    {"RegisterEndpoint", EvidenceKind::RegisterEndpoint},
    {"Attach", EvidenceKind::Attach},
    {"Detach", EvidenceKind::Detach},
    {"Move", EvidenceKind::Move},
    {"PublishObjectCapability", EvidenceKind::PublishObjectCapability},
    {"PublishPortCapability", EvidenceKind::PublishPortCapability},
    {"SetLifecycle", EvidenceKind::SetLifecycle},
    {"ReincarnateObject", EvidenceKind::ReincarnateObject},
}};

constexpr std::array<std::pair<const char*, IngestDisposition>, 14> kDispositions = {{
    {"Accepted", IngestDisposition::Accepted},
    {"AcceptedFencedSource", IngestDisposition::AcceptedFencedSource},
    {"AcceptedFencedIncarnation", IngestDisposition::AcceptedFencedIncarnation},
    {"AcceptedPendingReference", IngestDisposition::AcceptedPendingReference},
    {"AcceptedPendingIncarnation", IngestDisposition::AcceptedPendingIncarnation},
    {"AcceptedSuperseded", IngestDisposition::AcceptedSuperseded},
    {"Duplicate", IngestDisposition::Duplicate},
    {"RefusedUnknownSource", IngestDisposition::RefusedUnknownSource},
    {"RefusedLabelConflict", IngestDisposition::RefusedLabelConflict},
    {"RefusedConflict", IngestDisposition::RefusedConflict},
    {"RefusedInvalid", IngestDisposition::RefusedInvalid},
    {"RefusedCapacity", IngestDisposition::RefusedCapacity},
    {"RefusedClosed", IngestDisposition::RefusedClosed},
    {"RefusedPersistence", IngestDisposition::RefusedPersistence},
}};

} // namespace

const char* to_string(EvidenceKind value) noexcept {
  for (const auto& entry : kEvidenceKinds) {
    if (entry.second == value) {
      return entry.first;
    }
  }
  return "RegisterObject";
}

bool parse_evidence_kind(std::string_view text, EvidenceKind& out) noexcept {
  for (const auto& entry : kEvidenceKinds) {
    if (text == entry.first) {
      out = entry.second;
      return true;
    }
  }
  return false;
}

EvidenceKind kind_of(const EvidencePayload& payload) noexcept {
  return std::visit(
      [](const auto& concrete) -> EvidenceKind {
        using Concrete = std::decay_t<decltype(concrete)>;
        if constexpr (std::is_same_v<Concrete, RegisterObjectPayload>) {
          return EvidenceKind::RegisterObject;
        } else if constexpr (std::is_same_v<Concrete, RegisterEndpointPayload>) {
          return EvidenceKind::RegisterEndpoint;
        } else if constexpr (std::is_same_v<Concrete, AttachPayload>) {
          return EvidenceKind::Attach;
        } else if constexpr (std::is_same_v<Concrete, DetachPayload>) {
          return EvidenceKind::Detach;
        } else if constexpr (std::is_same_v<Concrete, MovePayload>) {
          return EvidenceKind::Move;
        } else if constexpr (std::is_same_v<Concrete, PublishObjectCapabilityPayload>) {
          return EvidenceKind::PublishObjectCapability;
        } else if constexpr (std::is_same_v<Concrete, PublishPortCapabilityPayload>) {
          return EvidenceKind::PublishPortCapability;
        } else if constexpr (std::is_same_v<Concrete, SetLifecyclePayload>) {
          return EvidenceKind::SetLifecycle;
        } else {
          return EvidenceKind::ReincarnateObject;
        }
      },
      payload);
}

EvidenceKind kind_of(const Evidence& evidence) noexcept {
  return kind_of(evidence.payload);
}

EvidenceId new_evidence_id() noexcept {
  return EvidenceId(detail::random_id128());
}

const char* to_string(IngestDisposition value) noexcept {
  for (const auto& entry : kDispositions) {
    if (entry.second == value) {
      return entry.first;
    }
  }
  return "RefusedInvalid";
}

bool is_accepted(IngestDisposition value) noexcept {
  switch (value) {
    case IngestDisposition::Accepted:
    case IngestDisposition::AcceptedFencedSource:
    case IngestDisposition::AcceptedFencedIncarnation:
    case IngestDisposition::AcceptedPendingReference:
    case IngestDisposition::AcceptedPendingIncarnation:
    case IngestDisposition::AcceptedSuperseded:
    case IngestDisposition::Duplicate:
      return true;
    case IngestDisposition::RefusedUnknownSource:
    case IngestDisposition::RefusedLabelConflict:
    case IngestDisposition::RefusedConflict:
    case IngestDisposition::RefusedInvalid:
    case IngestDisposition::RefusedCapacity:
    case IngestDisposition::RefusedClosed:
    case IngestDisposition::RefusedPersistence:
      return false;
  }
  return false;
}

bool is_effective(IngestDisposition value) noexcept {
  return value == IngestDisposition::Accepted || value == IngestDisposition::Duplicate;
}

} // namespace cable_registry
