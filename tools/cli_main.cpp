// Cable Attachment Registry — inspection and publication command line tool.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The tool works against a local store (opening it in process) or against a
// running daemon over the framed transport. Every command goes through the
// same handle, so it behaves identically either way, and the script runner
// keeps one monotonic generation counter per publisher so a test can drive a
// realistic evidence stream from a shell.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"

namespace {

using namespace cable_registry;

struct Options {
  std::string store;
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  bool has_endpoint = false;
  bool no_fsync = false;
  bool salvage = false;
  bool verbose = false;
  bool quiet = false;
  /// When set, the CLI derives every observation time from this base instead of
  /// reading the wall clock, so a scripted publication is reproducible and two
  /// independent runs produce the same canonical snapshot digest.
  bool fixed_time = false;
  std::int64_t time_base = 0;
};

struct StreamState {
  SourceId id{};
  Incarnation incarnation{1};
  Generation generation{};
  std::string name;
};

[[noreturn]] void die(const std::string& message, int code = 2) {
  std::fprintf(stderr, "%s\n", message.c_str());
  std::exit(code);
}

std::uint64_t parse_u64(const std::string& text, const char* what) {
  if (text.empty()) {
    die(std::string("missing ") + what);
  }
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    die(std::string("could not read ") + what + " from '" + text + "'");
  }
  return static_cast<std::uint64_t>(value);
}

Id128 parse_id(const std::string& text, const char* what) {
  Id128 value{};
  if (!parse_hex_id(text, value) || value.is_nil()) {
    die(std::string("could not read a non-nil 128 bit ") + what + " from '" + text + "'");
  }
  return value;
}

std::string field(const std::vector<std::string>& fields, std::size_t index, const char* what) {
  if (index >= fields.size()) {
    die(std::string("missing ") + what);
  }
  return fields[index];
}

std::string rest_of(const std::vector<std::string>& fields, std::size_t from) {
  std::string joined;
  for (std::size_t index = from; index < fields.size(); ++index) {
    if (!joined.empty()) {
      joined += ' ';
    }
    joined += fields[index];
  }
  return joined;
}

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> fields;
  std::istringstream stream(line);
  std::string token;
  while (stream >> token) {
    fields.push_back(token);
  }
  return fields;
}

/// One registry, reached either in process or over the wire.
class Handle {
 public:
  Handle(std::unique_ptr<Registry> local, std::unique_ptr<ipc::Client> remote)
      : local_(std::move(local)), remote_(std::move(remote)) {}

  Outcome<OpenSessionResult> open_session(const SourceDescriptor& descriptor, Incarnation incarnation) {
    if (local_) {
      return local_->OpenSession(descriptor, incarnation);
    }
    return remote_->OpenSession(descriptor, incarnation);
  }

  Outcome<IngestResult> publish(const Evidence& evidence) {
    if (local_) {
      return local_->Ingest(evidence);
    }
    return remote_->Publish(evidence);
  }

  Outcome<PortView> query_port(PortRef port, ValidationPolicy policy) {
    if (local_) {
      return local_->QueryPort(port, policy);
    }
    return remote_->QueryPort(port, policy);
  }

  Outcome<ObjectView> query_object(ObjectId object) {
    if (local_) {
      return local_->QueryObject(object);
    }
    return remote_->QueryObject(object);
  }

  Outcome<ObjectHistory> history(ObjectId object) {
    if (local_) {
      return local_->QueryHistory(object);
    }
    return remote_->QueryHistory(object);
  }

  Outcome<InspectionView> inspect(std::uint32_t max_entries) {
    if (local_) {
      return local_->Inspect(max_entries);
    }
    return remote_->Inspect(max_entries);
  }

  Outcome<TopologySnapshot> snapshot() {
    if (local_) {
      return local_->Snapshot();
    }
    return remote_->Snapshot();
  }

  Outcome<Digest256> digest() {
    if (local_) {
      return local_->GraphDigest();
    }
    return remote_->GraphDigest();
  }

  Outcome<RegistryStats> stats() {
    if (local_) {
      return local_->Stats();
    }
    return remote_->Stats();
  }

  void close() {
    if (local_) {
      local_->Close();
    }
    if (remote_) {
      remote_->Close();
    }
  }

 private:
  std::unique_ptr<Registry> local_;
  std::unique_ptr<ipc::Client> remote_;
};

/// Drives a sequence of evidence records with one generation counter per
/// publisher.
class Publisher {
 public:
  Publisher(Handle& handle, Options& options) : handle_(handle), options_(options) {}

  /// Opens a publisher session. @p announce prints the outcome, which the
  /// open-source command always does because the disposition is the answer the
  /// operator asked for; the script runner stays quiet unless --verbose.
  void open_source(const SourceDescriptor& descriptor, Incarnation incarnation, bool announce) {
    Outcome<OpenSessionResult> result = handle_.open_session(descriptor, incarnation);
    if (!result.has_value()) {
      die(result.error().describe());
    }
    if (announce) {
      std::printf("%s\n", to_json(result.value()).c_str());
    }
    StreamState state;
    state.id = descriptor.id;
    state.incarnation = incarnation;
    state.name = descriptor.name;
    streams_[descriptor.id] = state;
    names_[descriptor.name] = descriptor.id;
    current_ = descriptor.id;
  }

  void use(const std::string& name) {
    const auto found = names_.find(name);
    if (found == names_.end()) {
      die("no source named '" + name + "' has been opened");
    }
    current_ = found->second;
  }

  [[nodiscard]] bool has_source() const { return !current_.is_nil(); }

  IngestResult publish(EvidencePayload payload) {
    if (current_.is_nil()) {
      die("open a source before publishing evidence");
    }
    StreamState& state = streams_[current_];
    state.generation.value += 1;
    Evidence evidence;
    evidence.header.id =
        EvidenceId::from_parts(state.id.value().hi ^ 0x00E71DE000000000ull, state.generation.value);
    evidence.header.source = state.id;
    evidence.header.incarnation = state.incarnation;
    evidence.header.generation = state.generation;
    evidence.header.observed_at =
        options_.fixed_time
            ? Timestamp{options_.time_base + static_cast<std::int64_t>(state.generation.value) * 1'000'000}
            : now_timestamp();
    evidence.header.provenance = ProvenanceClass::OperatorEntry;
    evidence.header.provenance_detail = "cable-registry-cli";
    evidence.payload = std::move(payload);

    Outcome<IngestResult> result = handle_.publish(evidence);
    if (!result.has_value()) {
      die(result.error().describe());
    }
    if (options_.verbose) {
      std::printf("%s\n", to_json(result.value()).c_str());
    }
    if (!result.value().accepted()) {
      std::fprintf(stderr,
                   "refused: %s: %s\n",
                   to_string(result.value().disposition),
                   result.value().detail.c_str());
      refused_ += 1;
    }
    ++published_;
    return result.value();
  }

  [[nodiscard]] std::uint64_t published() const noexcept { return published_; }
  [[nodiscard]] std::uint64_t refused() const noexcept { return refused_; }

  void run_script(const std::string& path);

 private:
  Handle& handle_;
  Options& options_;
  std::map<SourceId, StreamState> streams_;
  std::map<std::string, SourceId> names_;
  SourceId current_{};
  std::uint64_t published_ = 0;
  std::uint64_t refused_ = 0;
};

void usage() {
  std::fprintf(stderr,
               "usage: cable-registry-cli (--store <path> | --endpoint <host:port>) <command> [args]\n"
               "\n"
               "global options:\n"
               "  --store <path>      open the registry in this process against a durable log\n"
               "  --endpoint <h:p>    talk to a running cable-registry-daemon\n"
               "  --no-fsync          do not flush before acknowledging (local mode)\n"
               "  --salvage           open a damaged log by discarding its damaged tail\n"
               "  --verbose           print one JSON line per published record\n"
               "  --time-base <ns>    derive observation times from this Unix nanosecond base\n"
               "                      instead of the wall clock, so a script is reproducible\n"
               "  --live-only         answer with claims asserted in this registry lifetime only\n"
               "\n"
               "commands:\n"
               "  open-source <hex32> <name> <authority-class> <rank> [incarnation]\n"
               "  script <file>                  run a line oriented evidence script\n"
               "  attach <object> <side> <endpoint> <port> [slot] [object-incarnation]\n"
               "  detach <object> <side> <endpoint> <port> [slot] [object-incarnation]\n"
               "  move <object> <side> <from-endpoint> <from-port> <to-endpoint> <to-port>\n"
               "  reincarnate <object> <from-incarnation> [serial]\n"
               "  quarantine|release|remove|retire <object> [reason]\n"
               "  publish-object <object> <label> <sides> [connector] [media]\n"
               "  publish-endpoint <endpoint> <name> <ports> [max-attachments]\n"
               "  publish-port-capability <endpoint> <port> <max-attachments>\n"
               "  query-port <endpoint> <port> [--live-only]\n"
               "  query-object <object>\n"
               "  history <object>\n"
               "  conflicts | unknown | stale\n"
               "  inspect [max-entries]\n"
               "  stats | digest | snapshot [--out <file>] | serve\n"
               "\n"
               "script commands: source, use, object, endpoint, attach, detach, move,\n"
               "                 reincarnate, quarantine, release, remove, retire,\n"
               "                 port-capability\n");
}


ObjectDescriptor build_object(const std::vector<std::string>& fields) {
  ObjectDescriptor descriptor;
  descriptor.id = ObjectId(parse_id(field(fields, 1, "object identity"), "object identity"));
  descriptor.kind = ObjectKind::Cable;
  descriptor.physical_label = field(fields, 2, "physical label");
  descriptor.serial_like = "SN-" + descriptor.physical_label;
  descriptor.administrative_location = "unspecified";
  const std::uint64_t sides = parse_u64(field(fields, 3, "side count"), "side count");
  if (sides == 0 || sides > 64) {
    die("the side count must be between 1 and 64");
  }
  ObjectSideDescriptor side;
  if (fields.size() > 4) {
    if (!parse_connector_class(fields[4], side.connector)) {
      die("unknown connector class '" + fields[4] + "'");
    }
  }
  if (fields.size() > 5) {
    if (!parse_media_class(fields[5], side.media)) {
      die("unknown media class '" + fields[5] + "'");
    }
  }
  side.lanes = LaneCount{static_cast<std::uint16_t>(4)};
  descriptor.sides.assign(static_cast<std::size_t>(sides), side);
  descriptor.capability.connector = side.connector;
  descriptor.capability.media = side.media;
  descriptor.capability.lanes = LaneCount{static_cast<std::uint16_t>(4)};
  return descriptor;
}

EndpointDescriptor build_endpoint(const std::vector<std::string>& fields) {
  EndpointDescriptor descriptor;
  descriptor.id = EndpointId(parse_id(field(fields, 1, "endpoint identity"), "endpoint identity"));
  descriptor.kind = EndpointKind::Switch;
  descriptor.name = field(fields, 2, "endpoint name");
  descriptor.administrative_location = "unspecified";
  const std::uint64_t ports = parse_u64(field(fields, 3, "port count"), "port count");
  if (ports == 0 || ports > 4096) {
    die("the port count must be between 1 and 4096");
  }
  descriptor.port_count = static_cast<std::uint32_t>(ports);
  descriptor.default_port_capability.max_simultaneous_attachments = 1;
  if (fields.size() > 4) {
    descriptor.default_port_capability.max_simultaneous_attachments =
        static_cast<std::uint32_t>(parse_u64(fields[4], "max attachments"));
  }
  return descriptor;
}

PortRef build_port(const std::string& endpoint_text, const std::string& index_text) {
  PortRef port;
  port.endpoint = EndpointId(parse_id(endpoint_text, "endpoint identity"));
  port.index = PortIndex{static_cast<std::uint32_t>(parse_u64(index_text, "port index"))};
  return port;
}

void Publisher::run_script(const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    die("could not open the script '" + path + "'");
  }
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    const std::vector<std::string> fields = split(line);
    if (fields.empty()) {
      continue;
    }
    const std::string& command = fields[0];
    try {
      if (command == "source") {
        SourceDescriptor descriptor;
        descriptor.id = SourceId(parse_id(field(fields, 1, "source identity"), "source identity"));
        descriptor.name = field(fields, 2, "source name");
        if (!parse_authority_class(field(fields, 3, "authority class"), descriptor.authority.authority_class)) {
          die("unknown authority class '" + fields[3] + "'");
        }
        descriptor.authority.rank = static_cast<std::uint32_t>(parse_u64(field(fields, 4, "authority rank"), "rank"));
        descriptor.administrative_domain = "cli";
        Incarnation incarnation{1};
        if (fields.size() > 5) {
          incarnation.value = parse_u64(fields[5], "incarnation");
        }
        open_source(descriptor, incarnation, false);
      } else if (command == "use") {
        use(field(fields, 1, "source name"));
      } else if (command == "object") {
        publish(RegisterObjectPayload{build_object(fields)});
      } else if (command == "endpoint") {
        publish(RegisterEndpointPayload{build_endpoint(fields)});
      } else if (command == "attach") {
        AttachPayload payload;
        payload.subject.object = ObjectId(parse_id(field(fields, 1, "object identity"), "object"));
        payload.subject.side = SideIndex{static_cast<std::uint32_t>(parse_u64(field(fields, 2, "side"), "side"))};
        payload.port = build_port(field(fields, 3, "endpoint identity"), field(fields, 4, "port index"));
        if (fields.size() > 5) {
          payload.slot = AttachmentSlot{static_cast<std::uint32_t>(parse_u64(fields[5], "slot"))};
        }
        payload.object_incarnation = ObjectIncarnation{1};
        if (fields.size() > 6) {
          payload.object_incarnation.value = parse_u64(fields[6], "object incarnation");
        }
        publish(payload);
      } else if (command == "detach") {
        DetachPayload payload;
        payload.subject.object = ObjectId(parse_id(field(fields, 1, "object identity"), "object"));
        payload.subject.side = SideIndex{static_cast<std::uint32_t>(parse_u64(field(fields, 2, "side"), "side"))};
        payload.has_from_port = true;
        payload.from_port = build_port(field(fields, 3, "endpoint identity"), field(fields, 4, "port index"));
        if (fields.size() > 5) {
          payload.slot = AttachmentSlot{static_cast<std::uint32_t>(parse_u64(fields[5], "slot"))};
        }
        payload.object_incarnation = ObjectIncarnation{1};
        if (fields.size() > 6) {
          payload.object_incarnation.value = parse_u64(fields[6], "object incarnation");
        }
        publish(payload);
      } else if (command == "move") {
        MovePayload payload;
        payload.subject.object = ObjectId(parse_id(field(fields, 1, "object identity"), "object"));
        payload.subject.side = SideIndex{static_cast<std::uint32_t>(parse_u64(field(fields, 2, "side"), "side"))};
        payload.from = build_port(field(fields, 3, "from endpoint"), field(fields, 4, "from port"));
        payload.to = build_port(field(fields, 5, "to endpoint"), field(fields, 6, "to port"));
        payload.object_incarnation = ObjectIncarnation{1};
        publish(payload);
      } else if (command == "reincarnate") {
        ReincarnateObjectPayload payload;
        payload.object = ObjectId(parse_id(field(fields, 1, "object identity"), "object"));
        payload.from_incarnation.value = parse_u64(field(fields, 2, "from incarnation"), "from incarnation");
        if (fields.size() > 3) {
          payload.new_serial_like = fields[3];
        }
        payload.reason = "cli";
        publish(payload);
      } else if (command == "quarantine" || command == "release" || command == "remove" || command == "retire") {
        SetLifecyclePayload payload;
        payload.object = ObjectId(parse_id(field(fields, 1, "object identity"), "object"));
        payload.object_incarnation = ObjectIncarnation{1};
        if (command == "quarantine") {
          payload.action = LifecycleAction::Quarantined;
        } else if (command == "release") {
          payload.action = LifecycleAction::Present;
        } else if (command == "remove") {
          payload.action = LifecycleAction::Removed;
        } else {
          payload.action = LifecycleAction::Retired;
        }
        payload.reason = rest_of(fields, 2);
        publish(payload);
      } else if (command == "port-capability") {
        PublishPortCapabilityPayload payload;
        payload.port = build_port(field(fields, 1, "endpoint identity"), field(fields, 2, "port index"));
        payload.capability.max_simultaneous_attachments =
            static_cast<std::uint32_t>(parse_u64(field(fields, 3, "max attachments"), "max attachments"));
        publish(payload);
      } else {
        die("unknown script command '" + command + "' on line " + std::to_string(line_number));
      }
    } catch (const std::string& message) {
      die("line " + std::to_string(line_number) + ": " + message);
    }
  }
}

void print_or_die(const Outcome<TopologySnapshot>& value) {
  if (!value.has_value()) {
    die(value.error().describe(), 1);
  }
  std::printf("%s\n", to_json(value.value()).c_str());
}

} // namespace

int main(int argc, char** argv) {
  Options options;
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--store" && index + 1 < argc) {
      options.store = argv[++index];
    } else if (argument == "--endpoint" && index + 1 < argc) {
      const std::string text = argv[++index];
      const std::size_t colon = text.rfind(':');
      if (colon == std::string::npos) {
        die("--endpoint expects host:port");
      }
      options.host = text.substr(0, colon);
      options.port = static_cast<std::uint16_t>(parse_u64(text.substr(colon + 1), "port"));
      options.has_endpoint = true;
    } else if (argument == "--no-fsync") {
      options.no_fsync = true;
    } else if (argument == "--salvage") {
      options.salvage = true;
    } else if (argument == "--verbose") {
      options.verbose = true;
    } else if (argument == "--quiet") {
      options.quiet = true;
    } else if (argument == "--time-base" && index + 1 < argc) {
      options.time_base = static_cast<std::int64_t>(parse_u64(argv[++index], "time base"));
      options.fixed_time = true;
    } else if (argument == "--live-only") {
      arguments.push_back(argument);
    } else if (argument == "--out" && index + 1 < argc) {
      arguments.push_back(argument);
      arguments.push_back(argv[++index]);
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    } else {
      arguments.push_back(argument);
    }
  }

  if (options.store.empty() && !options.has_endpoint) {
    usage();
    return 2;
  }
  if (!options.store.empty() && options.has_endpoint) {
    die("choose either --store or --endpoint, not both");
  }

  std::unique_ptr<Registry> local;
  std::unique_ptr<ipc::Client> remote;
  if (!options.store.empty()) {
    RegistryOptions registry_options;
    registry_options.store = StoreOptions{};
    registry_options.store->path = options.store;
    registry_options.store->fsync_on_commit = !options.no_fsync;
    registry_options.store->recovery_policy =
        options.salvage ? RecoveryPolicy::SalvagePrefix : RecoveryPolicy::Strict;
    Outcome<std::unique_ptr<Registry>> opened = Registry::Open(registry_options);
    if (!opened.has_value()) {
      die(opened.error().describe(), 1);
    }
    local = std::move(opened).value();
  } else {
    ipc::ClientOptions client_options;
    client_options.host = options.host;
    client_options.port = options.port;
    Outcome<std::unique_ptr<ipc::Client>> connected = ipc::Client::Connect(client_options);
    if (!connected.has_value()) {
      die(connected.error().describe(), 1);
    }
    remote = std::move(connected).value();
  }

  Handle handle(std::move(local), std::move(remote));
  Publisher publisher(handle, options);

  if (arguments.empty()) {
    usage();
    return 2;
  }

  const std::string command = arguments[0];
  std::vector<std::string> rest(arguments.begin() + 1, arguments.end());
  const bool live_only =
      std::find(rest.begin(), rest.end(), "--live-only") != rest.end();
  rest.erase(std::remove(rest.begin(), rest.end(), "--live-only"), rest.end());
  const ValidationPolicy policy = live_only ? ValidationPolicy::LiveOnly : ValidationPolicy::IncludeAll;

  auto need_source = [&] {
    if (!publisher.has_source()) {
      die("open a source first: the CLI cannot publish evidence without one");
    }
  };
  (void)need_source;

  if (command == "open-source") {
    SourceDescriptor descriptor;
    descriptor.id = SourceId(parse_id(field(rest, 0, "source identity"), "source identity"));
    descriptor.name = field(rest, 1, "source name");
    if (!parse_authority_class(field(rest, 2, "authority class"), descriptor.authority.authority_class)) {
      die("unknown authority class '" + rest[2] + "'");
    }
    descriptor.authority.rank = static_cast<std::uint32_t>(parse_u64(field(rest, 3, "authority rank"), "rank"));
    descriptor.administrative_domain = "cli";
    Incarnation incarnation{1};
    if (rest.size() > 4) {
      incarnation.value = parse_u64(rest[4], "incarnation");
    }
    publisher.open_source(descriptor, incarnation, true);
  } else if (command == "script") {
    publisher.run_script(field(rest, 0, "script path"));
    std::printf("{\"published\":%llu,\"refused\":%llu}\n",
                static_cast<unsigned long long>(publisher.published()),
                static_cast<unsigned long long>(publisher.refused()));
    if (publisher.refused() != 0) {
      handle.close();
      return 1;
    }
  } else if (command == "attach" || command == "detach" || command == "move" || command == "reincarnate" ||
             command == "quarantine" || command == "release" || command == "remove" || command == "retire" ||
             command == "publish-object" || command == "publish-endpoint" || command == "publish-port-capability" ||
             command == "publish-object-capability" || command == "publish-source") {
    std::vector<std::string> fields;
    fields.push_back(std::string());
    fields.insert(fields.end(), rest.begin(), rest.end());
    if (command == "publish-object") {
      publisher.publish(RegisterObjectPayload{build_object(fields)});
    } else if (command == "publish-endpoint") {
      publisher.publish(RegisterEndpointPayload{build_endpoint(fields)});
    } else if (command == "attach" || command == "detach" || command == "move" || command == "reincarnate" ||
               command == "quarantine" || command == "release" || command == "remove" || command == "retire" ||
               command == "publish-port-capability") {
      std::vector<std::string> script_line;
      script_line.push_back(command == "publish-port-capability" ? "port-capability" : command);
      script_line.insert(script_line.end(), rest.begin(), rest.end());
      const std::string joined = [&] {
        std::string text;
        for (const std::string& item : script_line) {
          if (!text.empty()) {
            text += ' ';
          }
          text += item;
        }
        return text;
      }();
      const std::string temporary = "cable-registry-cli-inline.script";
      {
        std::ofstream out(temporary, std::ios::trunc);
        out << joined << "\n";
      }
      publisher.run_script(temporary);
      std::remove(temporary.c_str());
    } else {
      die("unsupported command");
    }
  } else if (command == "query-port") {
    const PortRef port = build_port(field(rest, 0, "endpoint identity"), field(rest, 1, "port index"));
    const Outcome<PortView> view = handle.query_port(port, policy);
    if (!view.has_value()) {
      die(view.error().describe(), 1);
    }
    std::printf("%s\n", to_json(view.value()).c_str());
  } else if (command == "query-object") {
    const Outcome<ObjectView> view = handle.query_object(ObjectId(parse_id(field(rest, 0, "object identity"), "object")));
    if (!view.has_value()) {
      die(view.error().describe(), 1);
    }
    std::printf("%s\n", to_json(view.value()).c_str());
  } else if (command == "history") {
    const Outcome<ObjectHistory> history =
        handle.history(ObjectId(parse_id(field(rest, 0, "object identity"), "object")));
    if (!history.has_value()) {
      die(history.error().describe(), 1);
    }
    std::printf("%s\n", to_json(history.value()).c_str());
  } else if (command == "inspect" || command == "conflicts" || command == "unknown" || command == "stale") {
    std::uint32_t max_entries = 256;
    if (!rest.empty()) {
      max_entries = static_cast<std::uint32_t>(parse_u64(rest[0], "max entries"));
    }
    const Outcome<InspectionView> view = handle.inspect(max_entries);
    if (!view.has_value()) {
      die(view.error().describe(), 1);
    }
    if (command == "conflicts") {
      std::uint64_t count = 0;
      for (const PortView& port : view.value().unvalidated_ports) {
        count += port.conflicts.size();
      }
      std::printf("%s\n", to_json(view.value()).c_str());
      std::printf("{\"summarisedConflicts\":%llu}\n", static_cast<unsigned long long>(count));
    } else {
      std::printf("%s\n", to_json(view.value()).c_str());
    }
  } else if (command == "stats") {
    const Outcome<RegistryStats> stats = handle.stats();
    if (!stats.has_value()) {
      die(stats.error().describe(), 1);
    }
    std::printf("%s\n", to_json(stats.value()).c_str());
  } else if (command == "digest") {
    const Outcome<Digest256> digest = handle.digest();
    if (!digest.has_value()) {
      die(digest.error().describe(), 1);
    }
    std::printf("{\"graphDigest\":\"%s\"}\n", to_hex(digest.value()).c_str());
  } else if (command == "snapshot") {
    std::string output;
    for (std::size_t index = 0; index + 1 < rest.size(); ++index) {
      if (rest[index] == "--out") {
        output = rest[index + 1];
      }
    }
    const Outcome<TopologySnapshot> snapshot = handle.snapshot();
    if (!snapshot.has_value()) {
      die(snapshot.error().describe(), 1);
    }
    if (!output.empty()) {
      const std::vector<std::byte> encoded = snapshot.value().encode();
      std::ofstream out(output, std::ios::binary | std::ios::trunc);
      if (!out) {
        die("could not write '" + output + "'", 1);
      }
      out.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
      out.close();
      std::printf("{\"graphDigest\":\"%s\",\"bytes\":%llu}\n",
                  to_hex(snapshot.value().graph_digest()).c_str(),
                  static_cast<unsigned long long>(encoded.size()));
    } else {
      print_or_die(snapshot);
    }
  } else if (command == "serve") {
    const Outcome<Digest256> digest = handle.digest();
    if (!digest.has_value()) {
      die(digest.error().describe(), 1);
    }
    std::printf("{\"graphDigest\":\"%s\"}\n", to_hex(digest.value()).c_str());
  } else {
    usage();
    handle.close();
    return 2;
  }

  handle.close();
  return 0;
}
