// Cable Attachment Registry — policy validation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/limits.hpp"

#include <string>

namespace cable_registry {
namespace {

bool require_positive(std::uint32_t value, const char* name, std::string& message) {
  if (value == 0) {
    message = std::string("limits.") + name + " must be greater than zero";
    return false;
  }
  return true;
}

} // namespace

Outcome<void> validate_limits(const Limits& limits) {
  std::string message;
  if (!require_positive(limits.max_objects, "max_objects", message) ||
      !require_positive(limits.max_endpoints, "max_endpoints", message) ||
      !require_positive(limits.max_ports_per_endpoint, "max_ports_per_endpoint", message) ||
      !require_positive(limits.max_sides_per_object, "max_sides_per_object", message) ||
      !require_positive(limits.max_sources, "max_sources", message) ||
      !require_positive(limits.max_port_simultaneous_attachments, "max_port_simultaneous_attachments", message) ||
      !require_positive(limits.max_claim_records_per_port, "max_claim_records_per_port", message) ||
      !require_positive(limits.max_history_per_key, "max_history_per_key", message) ||
      !require_positive(limits.max_overridden_reported, "max_overridden_reported", message) ||
      !require_positive(limits.max_string_bytes, "max_string_bytes", message) ||
      !require_positive(limits.max_capability_code_bytes, "max_capability_code_bytes", message) ||
      !require_positive(limits.max_revalidate_claims, "max_revalidate_claims", message) ||
      !require_positive(limits.max_evidence_batch, "max_evidence_batch", message) ||
      !require_positive(limits.max_frame_payload_bytes, "max_frame_payload_bytes", message) ||
      !require_positive(limits.max_snapshot_entries, "max_snapshot_entries", message) ||
      !require_positive(limits.max_refusal_audit, "max_refusal_audit", message) ||
      !require_positive(limits.max_history_events, "max_history_events", message)) {
    return make_error(ErrorCode::InvalidArgument, std::move(message));
  }
  if (limits.max_string_bytes > (1u << 20)) {
    return make_error(ErrorCode::InvalidArgument, "limits.max_string_bytes exceeds the 1 MiB ceiling");
  }
  if (limits.max_frame_payload_bytes > (64u << 20)) {
    return make_error(ErrorCode::InvalidArgument, "limits.max_frame_payload_bytes exceeds the 64 MiB ceiling");
  }
  if (limits.max_port_simultaneous_attachments > 4096u) {
    return make_error(ErrorCode::InvalidArgument,
                      "limits.max_port_simultaneous_attachments exceeds the 4096 attachment ceiling");
  }
  return Outcome<void>();
}

} // namespace cable_registry
