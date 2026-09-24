// Cable Attachment Registry — JSON rendering of query results.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_RENDER_HPP
#define CABLE_REGISTRY_RENDER_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cable_registry/error.hpp"
#include "cable_registry/evidence.hpp"
#include "cable_registry/export.hpp"
#include "cable_registry/graph.hpp"
#include "cable_registry/persistence.hpp"
#include "cable_registry/registry.hpp"
#include "cable_registry/snapshot.hpp"

namespace cable_registry {

/// Minimal, deterministic JSON writer. Object keys are emitted in the order
/// the caller writes them, so a rendering is reproducible; it is a presentation
/// form and never an identity.
class CABLE_REGISTRY_API JsonWriter {
 public:
  explicit JsonWriter(bool pretty = true) : pretty_(pretty) {}

  void begin_object();
  void end_object();
  void begin_array();
  void end_array();
  void key(std::string_view name);

  void string(std::string_view value);
  void number(std::uint64_t value);
  void number(std::int64_t value);
  void number(double value);
  void boolean(bool value);
  void null();

  /// Inserts already rendered JSON verbatim. Only used to nest a document
  /// produced by this same writer; the argument must be well formed.
  void raw(std::string_view json);

  /// Writes @p name followed by @p value. The const char* overload exists so
  /// a string literal binds to the string form rather than to the bool
  /// overload through the standard pointer-to-bool conversion.
  void field(std::string_view name, const char* value);
  void field(std::string_view name, std::string_view value);
  void field(std::string_view name, std::uint64_t value);
  void field(std::string_view name, std::int64_t value);
  void field(std::string_view name, bool value);

  [[nodiscard]] const std::string& text() const noexcept { return out_; }
  [[nodiscard]] std::string take() { return std::move(out_); }

 private:
  void prepare_value();
  void indent();

  std::string out_;
  bool pretty_ = true;
  /// One entry per open container: true once it holds at least one member, so
  /// the closing brace knows whether to break the line.
  std::vector<bool> stack_;
  /// Set between a key and its value so the value does not emit a separator.
  bool after_key_ = false;
  int depth_ = 0;
};

CABLE_REGISTRY_API std::string to_json(const Id128& value);
CABLE_REGISTRY_API std::string to_json(const Digest256& value);
CABLE_REGISTRY_API std::string to_json(const Authority& value);
CABLE_REGISTRY_API std::string to_json(const NominalCapability& value);
CABLE_REGISTRY_API std::string to_json(const PortCapability& value);
CABLE_REGISTRY_API std::string to_json(const ClaimProvenance& value);
CABLE_REGISTRY_API std::string to_json(const ConflictNote& value);
CABLE_REGISTRY_API std::string to_json(const AttachmentEdge& value);
CABLE_REGISTRY_API std::string to_json(const PortView& value);
CABLE_REGISTRY_API std::string to_json(const ObjectSideView& value);
CABLE_REGISTRY_API std::string to_json(const ObjectView& value);
CABLE_REGISTRY_API std::string to_json(const EndpointView& value);
CABLE_REGISTRY_API std::string to_json(const TopologyView& value);
CABLE_REGISTRY_API std::string to_json(const GraphSummary& value);
CABLE_REGISTRY_API std::string to_json(const LifecycleEvent& value);
CABLE_REGISTRY_API std::string to_json(const ObjectHistory& value);
CABLE_REGISTRY_API std::string to_json(const FencedSessionView& value);
CABLE_REGISTRY_API std::string to_json(const FencedObjectView& value);
CABLE_REGISTRY_API std::string to_json(const PendingReferenceView& value);
CABLE_REGISTRY_API std::string to_json(const RefusalView& value);
CABLE_REGISTRY_API std::string to_json(const InspectionView& value);
CABLE_REGISTRY_API std::string to_json(const IngestResult& value);
CABLE_REGISTRY_API std::string to_json(const OpenSessionResult& value);
CABLE_REGISTRY_API std::string to_json(const SourceSessionView& value);
CABLE_REGISTRY_API std::string to_json(const RegistryStats& value);
CABLE_REGISTRY_API std::string to_json(const RecoveryReport& value);
CABLE_REGISTRY_API std::string to_json(const TopologySnapshot& value);
CABLE_REGISTRY_API std::string to_json(const SnapshotEnvelope& value);
CABLE_REGISTRY_API std::string to_json(const Error& value);

/// Parses the subset of JSON this library emits: an object of scalar fields.
/// Used by the command line tools to read publisher input. Returns false on
/// malformed input.
CABLE_REGISTRY_API bool parse_flat_json(std::string_view text, std::vector<std::pair<std::string, std::string>>& out);

} // namespace cable_registry

#endif // CABLE_REGISTRY_RENDER_HPP
