// Cable Attachment Registry — JSON rendering of query results.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Rendering is a presentation form, never an identity. The canonical identity
// of a graph is the binary snapshot encoding and its SHA-256 digest. This file
// exists so an operator, a script or a control-plane consumer can read an
// answer without linking the library.

#include "cable_registry/render.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cable_registry {
namespace {

void write_escaped(std::string& out, std::string_view text) {
  static const char kDigits[] = "0123456789abcdef";
  out += '"';
  for (const char character : text) {
    switch (character) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(character) < 0x20) {
          out += "\\u00";
          out += kDigits[(static_cast<unsigned char>(character) >> 4) & 0xFu];
          out += kDigits[static_cast<unsigned char>(character) & 0xFu];
        } else {
          out += character;
        }
        break;
    }
  }
  out += '"';
}

void write_authority(JsonWriter& writer, const Authority& value) {
  writer.field("class", to_string(value.authority_class));
  writer.field("rank", static_cast<std::uint64_t>(value.rank));
}

void write_capability(JsonWriter& writer, const NominalCapability& value) {
  writer.field("media", to_string(value.media));
  writer.field("connector", to_string(value.connector));
  writer.field("lanes", static_cast<std::uint64_t>(value.lanes.value));
  writer.field("nominalLaneRateKbps", value.nominal_lane_rate_kbps);
  writer.field("nominalTotalKbps", value.nominal_total_kbps);
  writer.field("nominalReachMm", value.nominal_reach_mm);
  writer.field("reachDeclared", value.reach_declared);
  writer.field("capabilityCode", value.capability_code);
}

void write_port_capability(JsonWriter& writer, const PortCapability& value) {
  writer.field("maxSimultaneousAttachments", static_cast<std::uint64_t>(value.max_simultaneous_attachments));
  writer.field("acceptedConnector", to_string(value.accepted_connector));
  writer.field("acceptedMedia", to_string(value.accepted_media));
  writer.field("laneCapacity", static_cast<std::uint64_t>(value.lane_capacity.value));
}

void write_provenance(JsonWriter& writer, const ClaimProvenance& value) {
  writer.field("source", value.source.to_hex());
  writer.field("incarnation", value.incarnation.value);
  writer.field("generation", value.generation.value);
  writer.field("evidence", value.evidence.to_hex());
  writer.field("observedAt", to_rfc3339_utc(value.observed_at));
  writer.field("receivedAt", to_rfc3339_utc(value.received_at));
  writer.field("provenance", to_string(value.provenance));
  writer.field("authorityClass", to_string(value.authority.authority_class));
  writer.field("authorityRank", static_cast<std::uint64_t>(value.authority.rank));
  writer.field("objectIncarnation", value.object_incarnation.value);
  writer.field("assertedInLifetime", value.asserted_in_lifetime);
}

void write_claim_array(JsonWriter& writer, std::string_view name, const std::vector<ClaimProvenance>& claims) {
  writer.key(name);
  writer.begin_array();
  for (const ClaimProvenance& claim : claims) {
    writer.begin_object();
    write_provenance(writer, claim);
    writer.end_object();
  }
  writer.end_array();
}

void write_note(JsonWriter& writer, const ConflictNote& value) {
  writer.begin_object();
  writer.field("reason", to_string(value.reason));
  writer.field("object", value.subject.object.to_hex());
  writer.field("side", static_cast<std::uint64_t>(value.subject.side.value));
  writer.field("endpoint", value.port.endpoint.to_hex());
  writer.field("port", static_cast<std::uint64_t>(value.port.index.value));
  writer.field("detail", value.detail);
  write_claim_array(writer, "claims", value.claims);
  writer.end_object();
}

void write_edge(JsonWriter& writer, const AttachmentEdge& value) {
  writer.begin_object();
  writer.field("object", value.subject.object.to_hex());
  writer.field("side", static_cast<std::uint64_t>(value.subject.side.value));
  writer.field("endpoint", value.port.endpoint.to_hex());
  writer.field("port", static_cast<std::uint64_t>(value.port.index.value));
  writer.field("slot", static_cast<std::uint64_t>(value.slot.value));
  writer.field("objectIncarnation", value.object_incarnation.value);
  writer.field("authorityClass", to_string(value.authority.authority_class));
  writer.field("authorityRank", static_cast<std::uint64_t>(value.authority.rank));
  writer.field("validation", to_string(value.validation));
  writer.field("connectorCompatible", value.connector_compatible);
  writer.field("mediaCompatible", value.media_compatible);
  writer.field("conflicting", value.conflicting);
  write_claim_array(writer, "claims", value.claims);
  write_claim_array(writer, "overridden", value.overridden);
  writer.field("overriddenTotal", static_cast<std::uint64_t>(value.overridden_total));
  writer.end_object();
}

void write_edge_array(JsonWriter& writer, std::string_view name, const std::vector<AttachmentEdge>& edges) {
  writer.key(name);
  writer.begin_array();
  for (const AttachmentEdge& edge : edges) {
    write_edge(writer, edge);
  }
  writer.end_array();
}

void write_note_array(JsonWriter& writer, std::string_view name, const std::vector<ConflictNote>& notes) {
  writer.key(name);
  writer.begin_array();
  for (const ConflictNote& note : notes) {
    write_note(writer, note);
  }
  writer.end_array();
}

void write_port_view(JsonWriter& writer, const PortView& value) {
  writer.begin_object();
  writer.field("endpoint", value.port.endpoint.to_hex());
  writer.field("port", static_cast<std::uint64_t>(value.port.index.value));
  writer.field("state", to_string(value.state));
  writer.field("validation", to_string(value.validation));
  writer.key("capability");
  writer.begin_object();
  write_port_capability(writer, value.capability);
  writer.end_object();
  write_edge_array(writer, "edges", value.edges);
  write_note_array(writer, "conflicts", value.conflicts);
  write_claim_array(writer, "emptinessClaims", value.emptiness_claims);
  writer.field("emptinessClaimsTotal", static_cast<std::uint64_t>(value.emptiness_claims_total));
  writer.end_object();
}

void write_object_side_view(JsonWriter& writer, const ObjectSideView& value) {
  writer.begin_object();
  writer.field("side", static_cast<std::uint64_t>(value.side.value));
  writer.field("connector", to_string(value.descriptor.connector));
  writer.field("media", to_string(value.descriptor.media));
  writer.field("lanes", static_cast<std::uint64_t>(value.descriptor.lanes.value));
  writer.field("state", to_string(value.state));
  writer.field("validation", to_string(value.validation));
  write_edge_array(writer, "edges", value.edges);
  write_note_array(writer, "conflicts", value.conflicts);
  writer.end_object();
}

void write_lifecycle_event(JsonWriter& writer, const LifecycleEvent& value) {
  writer.begin_object();
  writer.field("kind", to_string(value.kind));
  writer.field("observedAt", to_rfc3339_utc(value.observed_at));
  writer.field("receivedAt", to_rfc3339_utc(value.received_at));
  writer.field("source", value.source.to_hex());
  writer.field("incarnation", value.incarnation.value);
  writer.field("generation", value.generation.value);
  writer.field("evidence", value.evidence.to_hex());
  writer.field("provenance", to_string(value.provenance));
  writer.field("objectIncarnation", value.object_incarnation.value);
  writer.field("side", static_cast<std::uint64_t>(value.side.value));
  writer.field("endpoint", value.port.endpoint.to_hex());
  writer.field("port", static_cast<std::uint64_t>(value.port.index.value));
  writer.field("fromEndpoint", value.from_port.endpoint.to_hex());
  writer.field("fromPort", static_cast<std::uint64_t>(value.from_port.index.value));
  writer.field("hasFromPort", value.has_from_port);
  writer.field("detail", value.detail);
  writer.end_object();
}

void write_object_view(JsonWriter& writer, const ObjectView& value) {
  writer.begin_object();
  writer.field("object", value.id.to_hex());
  writer.field("kind", to_string(value.kind));
  writer.field("physicalLabel", value.physical_label);
  writer.field("serialLike", value.serial_like);
  writer.field("administrativeLocation", value.administrative_location);
  writer.field("incarnation", value.incarnation.value);
  writer.field("lifecycle", to_string(value.lifecycle));
  writer.field("validation", to_string(value.validation));
  writer.key("capability");
  writer.begin_object();
  write_capability(writer, value.capability);
  writer.end_object();
  writer.key("sides");
  writer.begin_array();
  for (const ObjectSideView& side : value.sides) {
    write_object_side_view(writer, side);
  }
  writer.end_array();
  writer.key("history");
  writer.begin_array();
  for (const LifecycleEvent& event : value.history) {
    write_lifecycle_event(writer, event);
  }
  writer.end_array();
  writer.field("historyDropped", static_cast<std::uint64_t>(value.history_dropped));
  write_note_array(writer, "conflicts", value.conflicts);
  writer.end_object();
}

void write_snapshot_claim(JsonWriter& writer, const SnapshotClaim& value) {
  writer.begin_object();
  writer.field("source", value.source.to_hex());
  writer.field("incarnation", value.incarnation.value);
  writer.field("generation", value.generation.value);
  writer.field("evidence", value.evidence.to_hex());
  writer.field("observedAt", to_rfc3339_utc(value.observed_at));
  writer.field("provenance", to_string(value.provenance));
  writer.field("authorityClass", to_string(value.authority.authority_class));
  writer.field("authorityRank", static_cast<std::uint64_t>(value.authority.rank));
  writer.end_object();
}

void write_snapshot_attachment(JsonWriter& writer, const SnapshotAttachment& value) {
  writer.begin_object();
  writer.field("object", value.subject.object.to_hex());
  writer.field("side", static_cast<std::uint64_t>(value.subject.side.value));
  writer.field("endpoint", value.port.endpoint.to_hex());
  writer.field("port", static_cast<std::uint64_t>(value.port.index.value));
  writer.field("slot", static_cast<std::uint64_t>(value.slot.value));
  writer.field("objectIncarnation", value.object_incarnation.value);
  writer.field("authorityClass", to_string(value.authority.authority_class));
  writer.field("authorityRank", static_cast<std::uint64_t>(value.authority.rank));
  writer.field("connectorCompatible", value.connector_compatible);
  writer.field("mediaCompatible", value.media_compatible);
  writer.field("conflicting", value.conflicting);
  writer.key("claims");
  writer.begin_array();
  for (const SnapshotClaim& claim : value.claims) {
    write_snapshot_claim(writer, claim);
  }
  writer.end_array();
  writer.end_object();
}

} // namespace

void JsonWriter::prepare_value() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (stack_.empty()) {
    return;
  }
  if (stack_.back()) {
    out_ += ',';
  }
  if (pretty_) {
    out_ += '\n';
    indent();
  }
  stack_.back() = true;
}

void JsonWriter::indent() {
  for (int level = 0; level < depth_; ++level) {
    out_ += "  ";
  }
}

void JsonWriter::begin_object() {
  prepare_value();
  out_ += '{';
  stack_.push_back(false);
  ++depth_;
}

void JsonWriter::end_object() {
  const bool had_members = !stack_.empty() && stack_.back();
  if (!stack_.empty()) {
    stack_.pop_back();
  }
  --depth_;
  if (had_members && pretty_) {
    out_ += '\n';
    indent();
  }
  out_ += '}';
}

void JsonWriter::begin_array() {
  prepare_value();
  out_ += '[';
  stack_.push_back(false);
  ++depth_;
}

void JsonWriter::end_array() {
  const bool had_members = !stack_.empty() && stack_.back();
  if (!stack_.empty()) {
    stack_.pop_back();
  }
  --depth_;
  if (had_members && pretty_) {
    out_ += '\n';
    indent();
  }
  out_ += ']';
}

void JsonWriter::key(std::string_view name) {
  if (!stack_.empty()) {
    if (stack_.back()) {
      out_ += ',';
    }
    if (pretty_) {
      out_ += '\n';
      indent();
    }
    stack_.back() = true;
  }
  write_escaped(out_, name);
  out_ += ':';
  if (pretty_) {
    out_ += ' ';
  }
  after_key_ = true;
}

void JsonWriter::string(std::string_view value) {
  prepare_value();
  write_escaped(out_, value);
}

void JsonWriter::number(std::uint64_t value) {
  prepare_value();
  out_ += std::to_string(value);
}

void JsonWriter::number(std::int64_t value) {
  prepare_value();
  out_ += std::to_string(value);
}

void JsonWriter::number(double value) {
  prepare_value();
  out_ += std::to_string(value);
}

void JsonWriter::boolean(bool value) {
  prepare_value();
  out_ += value ? "true" : "false";
}

void JsonWriter::null() {
  prepare_value();
  out_ += "null";
}

void JsonWriter::raw(std::string_view json) {
  prepare_value();
  out_ += json;
}

void JsonWriter::field(std::string_view name, const char* value) {
  key(name);
  string(std::string_view(value == nullptr ? "" : value));
}

void JsonWriter::field(std::string_view name, std::string_view value) {
  key(name);
  string(value);
}

void JsonWriter::field(std::string_view name, std::uint64_t value) {
  key(name);
  number(value);
}

void JsonWriter::field(std::string_view name, std::int64_t value) {
  key(name);
  number(value);
}

void JsonWriter::field(std::string_view name, bool value) {
  key(name);
  boolean(value);
}

std::string to_json(const Id128& value) {
  return std::string("\"") + to_hex(value) + "\"";
}

std::string to_json(const Digest256& value) {
  return std::string("\"") + to_hex(value) + "\"";
}

std::string to_json(const Authority& value) {
  JsonWriter writer;
  writer.begin_object();
  write_authority(writer, value);
  writer.end_object();
  return writer.take();
}

std::string to_json(const NominalCapability& value) {
  JsonWriter writer;
  writer.begin_object();
  write_capability(writer, value);
  writer.end_object();
  return writer.take();
}

std::string to_json(const PortCapability& value) {
  JsonWriter writer;
  writer.begin_object();
  write_port_capability(writer, value);
  writer.end_object();
  return writer.take();
}

std::string to_json(const ClaimProvenance& value) {
  JsonWriter writer;
  writer.begin_object();
  write_provenance(writer, value);
  writer.end_object();
  return writer.take();
}

std::string to_json(const ConflictNote& value) {
  JsonWriter writer;
  write_note(writer, value);
  return writer.take();
}

std::string to_json(const AttachmentEdge& value) {
  JsonWriter writer;
  write_edge(writer, value);
  return writer.take();
}

std::string to_json(const PortView& value) {
  JsonWriter writer;
  write_port_view(writer, value);
  return writer.take();
}

std::string to_json(const ObjectSideView& value) {
  JsonWriter writer;
  write_object_side_view(writer, value);
  return writer.take();
}

std::string to_json(const ObjectView& value) {
  JsonWriter writer;
  write_object_view(writer, value);
  return writer.take();
}

std::string to_json(const EndpointView& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("endpoint", value.id.to_hex());
  writer.field("kind", to_string(value.kind));
  writer.field("name", value.name);
  writer.field("administrativeLocation", value.administrative_location);
  writer.field("portCount", static_cast<std::uint64_t>(value.port_count));
  writer.key("defaultPortCapability");
  writer.begin_object();
  write_port_capability(writer, value.default_port_capability);
  writer.end_object();
  writer.key("ports");
  writer.begin_array();
  for (const PortView& port : value.ports) {
    write_port_view(writer, port);
  }
  writer.end_array();
  writer.end_object();
  return writer.take();
}

std::string to_json(const TopologyView& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("registry", value.registry.to_hex());
  writer.field("validationPolicy", to_string(value.validation_policy));
  writer.key("summary");
  writer.raw(to_json(value.summary));
  writer.key("objects");
  writer.begin_array();
  for (const ObjectView& object : value.objects) {
    write_object_view(writer, object);
  }
  writer.end_array();
  writer.key("endpoints");
  writer.begin_array();
  for (const EndpointView& endpoint : value.endpoints) {
    writer.begin_object();
    writer.field("endpoint", endpoint.id.to_hex());
    writer.field("kind", to_string(endpoint.kind));
    writer.field("name", endpoint.name);
    writer.field("administrativeLocation", endpoint.administrative_location);
    writer.field("portCount", static_cast<std::uint64_t>(endpoint.port_count));
    writer.end_object();
  }
  writer.end_array();
  writer.key("ports");
  writer.begin_array();
  for (const PortView& port : value.ports) {
    write_port_view(writer, port);
  }
  writer.end_array();
  writer.end_object();
  return writer.take();
}

std::string to_json(const GraphSummary& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("portsAttached", value.ports_attached);
  writer.field("portsEmpty", value.ports_empty);
  writer.field("portsUnknown", value.ports_unknown);
  writer.field("portsConflicting", value.ports_conflicting);
  writer.field("objectsTotal", value.objects_total);
  writer.field("objectsRegistered", value.objects_registered);
  writer.field("objectsUnattached", value.objects_unattached);
  writer.field("objectsAttached", value.objects_attached);
  writer.field("objectsQuarantined", value.objects_quarantined);
  writer.field("objectsRemoved", value.objects_removed);
  writer.field("objectsRetired", value.objects_retired);
  writer.field("edges", value.edges);
  writer.field("edgesUnvalidated", value.edges_unvalidated);
  writer.field("conflicts", value.conflicts);
  writer.field("claimsFenced", value.claims_fenced);
  writer.field("claimsPending", value.claims_pending);
  writer.end_object();
  return writer.take();
}

std::string to_json(const LifecycleEvent& value) {
  JsonWriter writer;
  write_lifecycle_event(writer, value);
  return writer.take();
}

std::string to_json(const ObjectHistory& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("object", value.object.to_hex());
  writer.key("events");
  writer.begin_array();
  for (const LifecycleEvent& event : value.events) {
    write_lifecycle_event(writer, event);
  }
  writer.end_array();
  writer.field("droppedEvents", static_cast<std::uint64_t>(value.dropped_events));
  writer.end_object();
  return writer.take();
}

std::string to_json(const FencedSessionView& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("source", value.source.to_hex());
  writer.field("incarnation", value.incarnation.value);
  writer.field("fencedBy", value.fenced_by.value);
  writer.field("claims", value.claims);
  writer.end_object();
  return writer.take();
}

std::string to_json(const FencedObjectView& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("object", value.object.to_hex());
  writer.field("physicalLabel", value.physical_label);
  writer.field("currentIncarnation", value.current_incarnation.value);
  writer.field("fencedClaims", value.fenced_claims);
  writer.end_object();
  return writer.take();
}

std::string to_json(const PendingReferenceView& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("evidence", value.evidence.to_hex());
  writer.field("kind", to_string(value.kind));
  writer.field("source", value.source.to_hex());
  writer.field("incarnation", value.incarnation.value);
  writer.field("generation", value.generation.value);
  writer.field("detail", value.detail);
  writer.end_object();
  return writer.take();
}

std::string to_json(const RefusalView& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("evidence", value.evidence.to_hex());
  writer.field("kind", to_string(value.kind));
  writer.field("source", value.source.to_hex());
  writer.field("incarnation", value.incarnation.value);
  writer.field("generation", value.generation.value);
  writer.field("disposition", to_string(value.disposition));
  writer.field("detail", value.detail);
  writer.end_object();
  return writer.take();
}

std::string to_json(const InspectionView& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.key("fencedSessions");
  writer.begin_array();
  for (const FencedSessionView& session : value.fenced_sessions) {
    writer.begin_object();
    writer.field("source", session.source.to_hex());
    writer.field("incarnation", session.incarnation.value);
    writer.field("fencedBy", session.fenced_by.value);
    writer.field("claims", session.claims);
    writer.end_object();
  }
  writer.end_array();
  writer.key("fencedObjects");
  writer.begin_array();
  for (const FencedObjectView& object : value.fenced_objects) {
    writer.begin_object();
    writer.field("object", object.object.to_hex());
    writer.field("physicalLabel", object.physical_label);
    writer.field("currentIncarnation", object.current_incarnation.value);
    writer.field("fencedClaims", object.fenced_claims);
    writer.end_object();
  }
  writer.end_array();
  writer.key("unvalidatedPorts");
  writer.begin_array();
  for (const PortView& port : value.unvalidated_ports) {
    write_port_view(writer, port);
  }
  writer.end_array();
  writer.key("pendingReferences");
  writer.begin_array();
  for (const PendingReferenceView& pending : value.pending_references) {
    writer.begin_object();
    writer.field("evidence", pending.evidence.to_hex());
    writer.field("kind", to_string(pending.kind));
    writer.field("source", pending.source.to_hex());
    writer.field("incarnation", pending.incarnation.value);
    writer.field("generation", pending.generation.value);
    writer.field("detail", pending.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.key("refusals");
  writer.begin_array();
  for (const RefusalView& refusal : value.refusals) {
    writer.begin_object();
    writer.field("evidence", refusal.evidence.to_hex());
    writer.field("kind", to_string(refusal.kind));
    writer.field("source", refusal.source.to_hex());
    writer.field("incarnation", refusal.incarnation.value);
    writer.field("generation", refusal.generation.value);
    writer.field("disposition", to_string(refusal.disposition));
    writer.field("detail", refusal.detail);
    writer.end_object();
  }
  writer.end_array();
  writer.key("summary");
  writer.raw(to_json(value.summary));
  writer.end_object();
  return writer.take();
}

std::string to_json(const IngestResult& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("evidence", value.evidence.to_hex());
  writer.field("kind", to_string(value.kind));
  writer.field("disposition", to_string(value.disposition));
  writer.field("accepted", value.accepted());
  writer.field("effective", value.effective());
  writer.field("highWater", value.high_water.value);
  writer.field("receivedAt", to_rfc3339_utc(value.received_at));
  writer.field("detail", value.detail);
  writer.end_object();
  return writer.take();
}

std::string to_json(const OpenSessionResult& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("disposition", to_string(value.disposition));
  writer.field("source", value.key.source.to_hex());
  writer.field("incarnation", value.key.incarnation.value);
  writer.field("previousIncarnation", value.previous_incarnation.value);
  writer.field("fencedClaims", value.fenced_claims);
  writer.field("detail", value.detail);
  writer.end_object();
  return writer.take();
}

std::string to_json(const SourceSessionView& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("source", value.key.source.to_hex());
  writer.field("incarnation", value.key.incarnation.value);
  writer.field("name", value.name);
  writer.field("authorityClass", to_string(value.authority.authority_class));
  writer.field("authorityRank", static_cast<std::uint64_t>(value.authority.rank));
  writer.field("live", value.live);
  writer.field("fenced", value.fenced);
  writer.field("fencedBy", value.fenced_by.value);
  writer.field("highWater", value.high_water.value);
  writer.field("openedAt", to_rfc3339_utc(value.opened_at));
  writer.field("acceptedRecords", value.accepted_records);
  writer.field("refusedRecords", value.refused_records);
  writer.field("duplicateRecords", value.duplicate_records);
  writer.field("missingGenerations", value.missing_generations);
  writer.end_object();
  return writer.take();
}

std::string to_json(const RegistryStats& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("registry", value.registry.to_hex());
  writer.field("openedAt", to_rfc3339_utc(value.opened_at));
  writer.field("objects", value.objects);
  writer.field("endpoints", value.endpoints);
  writer.field("ports", value.ports);
  writer.field("sources", value.sources);
  writer.field("sessions", value.sessions);
  writer.field("liveSessions", value.live_sessions);
  writer.field("claimRecords", value.claim_records);
  writer.field("portRecords", value.port_records);
  writer.field("lifecycleRecords", value.lifecycle_records);
  writer.field("capabilityRecords", value.capability_records);
  writer.field("evidenceAccepted", value.evidence_accepted);
  writer.field("evidenceRefused", value.evidence_refused);
  writer.field("evidenceDuplicate", value.evidence_duplicate);
  writer.field("evidenceSuperseded", value.evidence_superseded);
  writer.field("evidenceFenced", value.evidence_fenced);
  writer.field("evidencePending", value.evidence_pending);
  writer.field("storeRecords", value.store_records);
  writer.field("storeBytes", value.store_bytes);
  writer.field("storeRewrites", value.store_rewrites);
  writer.key("recovery");
  writer.begin_object();
  writer.field("opened", value.recovery.opened);
  writer.field("created", value.recovery.created);
  writer.field("salvaged", value.recovery.salvaged);
  writer.field("chainIntact", value.recovery.chain_intact);
  writer.field("formatVersion", static_cast<std::uint64_t>(value.recovery.format_version));
  writer.field("recordsLoaded", value.recovery.records_loaded);
  writer.field("recordsRejected", value.recovery.records_rejected);
  writer.field("bytesLoaded", value.recovery.bytes_loaded);
  writer.field("bytesDiscarded", value.recovery.bytes_discarded);
  writer.field("issue", to_string(value.recovery.issue));
  writer.field("issueOffset", value.recovery.issue_offset);
  writer.field("detail", value.recovery.detail);
  writer.end_object();
  writer.end_object();
  return writer.take();
}

std::string to_json(const RecoveryReport& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("opened", value.opened);
  writer.field("created", value.created);
  writer.field("salvaged", value.salvaged);
  writer.field("chainIntact", value.chain_intact);
  writer.field("formatVersion", static_cast<std::uint64_t>(value.format_version));
  writer.field("recordsLoaded", value.records_loaded);
  writer.field("recordsRejected", value.records_rejected);
  writer.field("bytesLoaded", value.bytes_loaded);
  writer.field("bytesDiscarded", value.bytes_discarded);
  writer.field("issue", to_string(value.issue));
  writer.field("issueOffset", value.issue_offset);
  writer.field("detail", value.detail);
  writer.end_object();
  return writer.take();
}

std::string to_json(const TopologySnapshot& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("formatVersion", static_cast<std::uint64_t>(value.format_version));
  writer.field("registry", value.registry.to_hex());
  writer.field("provenanceDigest", to_hex(value.provenance_digest));
  writer.field("digest", to_hex(value.digest()));
  writer.field("graphDigest", to_hex(value.graph_digest()));
  writer.key("summary");
  writer.raw(to_json(value.summary));
  writer.key("objects");
  writer.begin_array();
  for (const SnapshotObject& object : value.objects) {
    writer.begin_object();
    writer.field("object", object.id.to_hex());
    writer.field("kind", to_string(object.kind));
    writer.field("physicalLabel", object.physical_label);
    writer.field("serialLike", object.serial_like);
    writer.field("administrativeLocation", object.administrative_location);
    writer.field("incarnation", object.incarnation.value);
    writer.field("lifecycle", to_string(object.lifecycle));
    writer.key("capability");
    writer.begin_object();
    write_capability(writer, object.capability);
    writer.end_object();
    writer.key("sides");
    writer.begin_array();
    for (const ObjectSideDescriptor& side : object.sides) {
      writer.begin_object();
      writer.field("connector", to_string(side.connector));
      writer.field("media", to_string(side.media));
      writer.field("lanes", static_cast<std::uint64_t>(side.lanes.value));
      writer.end_object();
    }
    writer.end_array();
    writer.key("attachments");
    writer.begin_array();
    for (const SnapshotAttachment& attachment : object.attachments) {
      write_snapshot_attachment(writer, attachment);
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();
  writer.key("endpoints");
  writer.begin_array();
  for (const SnapshotEndpoint& endpoint : value.endpoints) {
    writer.begin_object();
    writer.field("endpoint", endpoint.id.to_hex());
    writer.field("kind", to_string(endpoint.kind));
    writer.field("name", endpoint.name);
    writer.field("administrativeLocation", endpoint.administrative_location);
    writer.field("portCount", static_cast<std::uint64_t>(endpoint.port_count));
    writer.key("defaultPortCapability");
    writer.begin_object();
    write_port_capability(writer, endpoint.default_port_capability);
    writer.end_object();
    writer.end_object();
  }
  writer.end_array();
  writer.key("ports");
  writer.begin_array();
  for (const SnapshotPort& port : value.ports) {
    writer.begin_object();
    writer.field("endpoint", port.port.endpoint.to_hex());
    writer.field("port", static_cast<std::uint64_t>(port.port.index.value));
    writer.field("state", to_string(port.state));
    writer.key("capability");
    writer.begin_object();
    write_port_capability(writer, port.capability);
    writer.end_object();
    writer.key("attachments");
    writer.begin_array();
    for (const SnapshotAttachment& attachment : port.attachments) {
      write_snapshot_attachment(writer, attachment);
    }
    writer.end_array();
    writer.key("conflicts");
    writer.begin_array();
    for (const SnapshotConflict& conflict : port.conflicts) {
      writer.begin_object();
      writer.field("reason", to_string(conflict.reason));
      writer.field("object", conflict.subject.object.to_hex());
      writer.field("side", static_cast<std::uint64_t>(conflict.subject.side.value));
      writer.field("endpoint", conflict.port.endpoint.to_hex());
      writer.field("port", static_cast<std::uint64_t>(conflict.port.index.value));
      writer.key("claims");
      writer.begin_array();
      for (const SnapshotClaim& claim : conflict.claims) {
        write_snapshot_claim(writer, claim);
      }
      writer.end_array();
      writer.end_object();
    }
    writer.end_array();
    writer.end_object();
  }
  writer.end_array();
  writer.key("fencedSessions");
  writer.begin_array();
  for (const SnapshotFencedSession& session : value.fenced_sessions) {
    writer.begin_object();
    writer.field("source", session.source.to_hex());
    writer.field("incarnation", session.incarnation.value);
    writer.field("fencedBy", session.fenced_by.value);
    writer.field("claims", session.claims);
    writer.end_object();
  }
  writer.end_array();
  writer.key("fencedObjects");
  writer.begin_array();
  for (const SnapshotFencedObject& object : value.fenced_objects) {
    writer.begin_object();
    writer.field("object", object.object.to_hex());
    writer.field("currentIncarnation", object.current_incarnation.value);
    writer.field("fencedClaims", object.fenced_claims);
    writer.end_object();
  }
  writer.end_array();
  writer.end_object();
  return writer.take();
}

std::string to_json(const SnapshotEnvelope& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("generatedAt", to_rfc3339_utc(value.generated_at));
  writer.field("digest", to_hex(value.digest));
  writer.key("validation");
  writer.begin_object();
  writer.field("claimsLive", value.validation.claims_live);
  writer.field("claimsRecovered", value.validation.claims_recovered);
  writer.field("portsValidated", value.validation.ports_validated);
  writer.field("portsPartiallyValidated", value.validation.ports_partially_validated);
  writer.field("portsUnvalidated", value.validation.ports_unvalidated);
  writer.end_object();
  writer.key("snapshot");
  writer.raw(to_json(value.snapshot));
  writer.end_object();
  return writer.take();
}

std::string to_json(const Error& value) {
  JsonWriter writer;
  writer.begin_object();
  writer.field("code", to_string(value.code()));
  writer.field("message", value.message());
  writer.end_object();
  return writer.take();
}

bool parse_flat_json(std::string_view text, std::vector<std::pair<std::string, std::string>>& out) {
  out.clear();
  std::size_t index = 0;
  auto skip_space = [&]() {
    while (index < text.size() && (text[index] == ' ' || text[index] == '\t' || text[index] == '\n' ||
                                   text[index] == '\r')) {
      ++index;
    }
  };
  auto read_string = [&](std::string& value) {
    if (index >= text.size() || text[index] != '"') {
      return false;
    }
    ++index;
    value.clear();
    while (index < text.size() && text[index] != '"') {
      if (text[index] == '\\' && index + 1 < text.size()) {
        ++index;
        switch (text[index]) {
          case 'n':
            value += '\n';
            break;
          case 't':
            value += '\t';
            break;
          case 'r':
            value += '\r';
            break;
          case '"':
            value += '"';
            break;
          case '\\':
            value += '\\';
            break;
          default:
            value += text[index];
            break;
        }
        ++index;
        continue;
      }
      value += text[index];
      ++index;
    }
    if (index >= text.size()) {
      return false;
    }
    ++index; // closing quote
    return true;
  };
  auto read_scalar = [&](std::string& value) {
    const std::size_t start = index;
    while (index < text.size() && text[index] != ',' && text[index] != '}' && text[index] != ']') {
      ++index;
    }
    std::size_t end = index;
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' ||
                           text[end - 1] == '\r')) {
      --end;
    }
    value.assign(text.substr(start, end - start));
    return true;
  };

  skip_space();
  if (index >= text.size() || text[index] != '{') {
    return false;
  }
  ++index;
  skip_space();
  if (index < text.size() && text[index] == '}') {
    return true;
  }
  while (index < text.size()) {
    skip_space();
    std::string name;
    if (!read_string(name)) {
      return false;
    }
    skip_space();
    if (index >= text.size() || text[index] != ':') {
      return false;
    }
    ++index;
    skip_space();
    std::string value;
    if (index < text.size() && text[index] == '"') {
      if (!read_string(value)) {
        return false;
      }
    } else if (!read_scalar(value)) {
      return false;
    }
    out.emplace_back(std::move(name), std::move(value));
    skip_space();
    if (index < text.size() && text[index] == ',') {
      ++index;
      continue;
    }
    if (index < text.size() && text[index] == '}') {
      return true;
    }
    return false;
  }
  return false;
}

} // namespace cable_registry
