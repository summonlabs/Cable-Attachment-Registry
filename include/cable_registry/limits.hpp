// Cable Attachment Registry — bounded resource policy.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_LIMITS_HPP
#define CABLE_REGISTRY_LIMITS_HPP

#include <cstdint>

#include "cable_registry/error.hpp"
#include "cable_registry/export.hpp"

namespace cable_registry {

/// Every externally derived size is validated against this policy before an
/// allocation or a graph expansion happens. The defaults are deliberately
/// generous but finite: the registry refuses work it cannot bound instead of
/// growing without limit.
struct Limits {
  /// Registered physical objects.
  std::uint32_t max_objects = 1'000'000;
  /// Registered endpoints.
  std::uint32_t max_endpoints = 100'000;
  /// Ports an endpoint may declare.
  std::uint32_t max_ports_per_endpoint = 4'096;
  /// Connector positions a physical object may declare.
  std::uint32_t max_sides_per_object = 64;
  /// Registered evidence sources.
  std::uint32_t max_sources = 4'096;
  /// Upper bound on a port's declared multi-attachment capability.
  std::uint32_t max_port_simultaneous_attachments = 64;
  /// Port-keyed claim records tolerated for one port before the registry
  /// refuses further distinct sources on it.
  std::uint32_t max_claim_records_per_port = 4'096;
  /// Prior records retained per claim key for history queries.
  std::uint32_t max_history_per_key = 8;
  /// Overridden (lower authority) claims reported alongside one port.
  std::uint32_t max_overridden_reported = 16;
  /// Bytes of one label, serial, location or reason string.
  std::uint32_t max_string_bytes = 256;
  /// Bytes of one nominal capability code.
  std::uint32_t max_capability_code_bytes = 64;
  /// Claim keys one revalidation record may renew.
  std::uint32_t max_revalidate_claims = 4'096;
  /// Evidence records accepted in one batch call.
  std::uint32_t max_evidence_batch = 4'096;
  /// Bytes of one transport frame payload.
  std::uint32_t max_frame_payload_bytes = 1u << 20;
  /// Objects and ports one snapshot may encode.
  std::uint32_t max_snapshot_entries = 4'000'000;
  /// Refusals retained for inspection before the oldest is dropped.
  std::uint32_t max_refusal_audit = 1'024;
  /// Events returned by one history query.
  std::uint32_t max_history_events = 4'096;
};

/// Validates a policy. Every field must be non-zero and internally consistent.
CABLE_REGISTRY_API Outcome<void> validate_limits(const Limits& limits);

} // namespace cable_registry

#endif // CABLE_REGISTRY_LIMITS_HPP
