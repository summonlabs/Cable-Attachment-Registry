// Cable Attachment Registry — canonical binary encoding of domain values.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_SERIALIZATION_HPP
#define CABLE_REGISTRY_SERIALIZATION_HPP

#include <cstddef>
#include <span>
#include <vector>

#include "cable_registry/authority.hpp"
#include "cable_registry/codec.hpp"
#include "cable_registry/evidence.hpp"
#include "cable_registry/graph.hpp"
#include "cable_registry/limits.hpp"
#include "cable_registry/registry.hpp"
#include "cable_registry/snapshot.hpp"

namespace cable_registry {

// Every encode overload appends to the supplied Encoder; every decode overload
// consumes from the supplied Decoder and returns false on any malformed,
// truncated or out-of-bound content, leaving the destination untouched when it
// can. The free functions returning a byte vector are the wire helpers.

CABLE_REGISTRY_API void encode_source_descriptor(Encoder& out, const SourceDescriptor& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_source_descriptor(Decoder& in, const Limits& limits, SourceDescriptor& out);

CABLE_REGISTRY_API void encode_capability(Encoder& out, const NominalCapability& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_capability(Decoder& in, const Limits& limits, NominalCapability& out);
CABLE_REGISTRY_API void encode_port_capability(Encoder& out, const PortCapability& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_port_capability(Decoder& in, const Limits& limits, PortCapability& out);

CABLE_REGISTRY_API void encode_object_descriptor(Encoder& out, const ObjectDescriptor& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_object_descriptor(Decoder& in, const Limits& limits, ObjectDescriptor& out);
CABLE_REGISTRY_API void encode_endpoint_descriptor(Encoder& out, const EndpointDescriptor& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_endpoint_descriptor(Decoder& in, const Limits& limits, EndpointDescriptor& out);

CABLE_REGISTRY_API void encode_evidence(Encoder& out, const Evidence& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_evidence(Decoder& in, const Limits& limits, Evidence& out);
CABLE_REGISTRY_API std::vector<std::byte> encode_evidence(const Evidence& value, const Limits& limits);
CABLE_REGISTRY_API Outcome<Evidence> decode_evidence(std::span<const std::byte> data, const Limits& limits);

CABLE_REGISTRY_API void encode_claim_provenance(Encoder& out, const ClaimProvenance& value);
CABLE_REGISTRY_API bool decode_claim_provenance(Decoder& in, ClaimProvenance& out);
CABLE_REGISTRY_API void encode_conflict_note(Encoder& out, const ConflictNote& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_conflict_note(Decoder& in, const Limits& limits, ConflictNote& out);
CABLE_REGISTRY_API void encode_attachment_edge(Encoder& out, const AttachmentEdge& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_attachment_edge(Decoder& in, const Limits& limits, AttachmentEdge& out);

CABLE_REGISTRY_API void encode_port_view(Encoder& out, const PortView& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_port_view(Decoder& in, const Limits& limits, PortView& out);
CABLE_REGISTRY_API void encode_object_view(Encoder& out, const ObjectView& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_object_view(Decoder& in, const Limits& limits, ObjectView& out);
CABLE_REGISTRY_API void encode_endpoint_view(Encoder& out, const EndpointView& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_endpoint_view(Decoder& in, const Limits& limits, EndpointView& out);
CABLE_REGISTRY_API void encode_lifecycle_event(Encoder& out, const LifecycleEvent& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_lifecycle_event(Decoder& in, const Limits& limits, LifecycleEvent& out);
CABLE_REGISTRY_API void encode_object_history(Encoder& out, const ObjectHistory& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_object_history(Decoder& in, const Limits& limits, ObjectHistory& out);

CABLE_REGISTRY_API void encode_ingest_result(Encoder& out, const IngestResult& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_ingest_result(Decoder& in, const Limits& limits, IngestResult& out);
CABLE_REGISTRY_API std::vector<std::byte> encode_ingest_results(std::span<const IngestResult> values,
                                                               const Limits& limits);
CABLE_REGISTRY_API bool decode_ingest_results(Decoder& in, const Limits& limits, std::vector<IngestResult>& out);

CABLE_REGISTRY_API void encode_inspection_view(Encoder& out, const InspectionView& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_inspection_view(Decoder& in, const Limits& limits, InspectionView& out);

CABLE_REGISTRY_API void encode_registry_stats(Encoder& out, const RegistryStats& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_registry_stats(Decoder& in, const Limits& limits, RegistryStats& out);
CABLE_REGISTRY_API void encode_open_session_result(Encoder& out, const OpenSessionResult& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_open_session_result(Decoder& in, const Limits& limits, OpenSessionResult& out);

CABLE_REGISTRY_API void encode_query_options(Encoder& out, const QueryOptions& value);
CABLE_REGISTRY_API bool decode_query_options(Decoder& in, QueryOptions& out);

CABLE_REGISTRY_API void encode_topology_snapshot(Encoder& out, const TopologySnapshot& value, const Limits& limits);
CABLE_REGISTRY_API bool decode_topology_snapshot(Decoder& in, const Limits& limits, TopologySnapshot& out);

} // namespace cable_registry

#endif // CABLE_REGISTRY_SERIALIZATION_HPP
