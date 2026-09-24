// Cable Attachment Registry — registry daemon.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Hosts one registry over the framed loopback transport. The process prints
// "READY <port>" on standard output once the listener is bound, so a test or a
// supervisor can wait for readiness without polling or guessing. It stops when
// a client asks it to, or when the process is terminated; there is no watchdog
// and no timeout anywhere.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"

namespace {

using namespace cable_registry;

void usage() {
  std::fprintf(stderr,
               "usage: cable-registry-daemon --store <path> [options]\n"
               "\n"
               "  --store <path>          durable log file (required)\n"
               "  --port <n>              TCP port, 0 for an ephemeral one (default 0)\n"
               "  --bind <address>        interface to bind (default 127.0.0.1)\n"
               "  --max-connections <n>   connections served at once (default 32)\n"
               "  --no-fsync              do not flush to stable storage before acknowledging\n"
               "  --salvage               open a damaged log by discarding its damaged tail\n"
               "  --no-shutdown           refuse a remote shutdown request\n"
               "  --quiet                 do not print the readiness line\n");
}

bool parse_u32(const char* text, std::uint32_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (end == nullptr || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

} // namespace

int main(int argc, char** argv) {
  std::string store_path;
  std::string bind_address = "127.0.0.1";
  std::uint32_t port = 0;
  std::uint32_t max_connections = 32;
  bool fsync_on_commit = true;
  bool salvage = false;
  bool allow_shutdown = true;
  bool quiet = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto next = [&](const char*& value) {
      if (index + 1 >= argc) {
        return false;
      }
      value = argv[++index];
      return true;
    };
    const char* value = nullptr;
    if (argument == "--store") {
      if (!next(value)) {
        usage();
        return 2;
      }
      store_path = value;
    } else if (argument == "--bind") {
      if (!next(value)) {
        usage();
        return 2;
      }
      bind_address = value;
    } else if (argument == "--port") {
      if (!next(value) || !parse_u32(value, port)) {
        usage();
        return 2;
      }
    } else if (argument == "--max-connections") {
      if (!next(value) || !parse_u32(value, max_connections)) {
        usage();
        return 2;
      }
    } else if (argument == "--no-fsync") {
      fsync_on_commit = false;
    } else if (argument == "--salvage") {
      salvage = true;
    } else if (argument == "--no-shutdown") {
      allow_shutdown = false;
    } else if (argument == "--quiet") {
      quiet = true;
    } else if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
      usage();
      return 2;
    }
  }

  if (store_path.empty()) {
    usage();
    return 2;
  }

  RegistryOptions options;
  options.store = StoreOptions{};
  options.store->path = store_path;
  options.store->fsync_on_commit = fsync_on_commit;
  options.store->recovery_policy = salvage ? RecoveryPolicy::SalvagePrefix : RecoveryPolicy::Strict;

  Outcome<std::unique_ptr<Registry>> registry = Registry::Open(options);
  if (!registry.has_value()) {
    std::fprintf(stderr, "the registry could not be opened: %s\n", registry.error().describe().c_str());
    return 1;
  }

  const RecoveryReport recovery = registry.value()->recovery_report();
  if (!quiet) {
    std::printf("RECOVERY %s records=%llu discarded=%llu salvaged=%s\n",
                to_string(recovery.issue),
                static_cast<unsigned long long>(recovery.records_loaded),
                static_cast<unsigned long long>(recovery.bytes_discarded),
                recovery.salvaged ? "yes" : "no");
  }

  ipc::ServerOptions server_options;
  server_options.bind_address = bind_address;
  server_options.port = static_cast<std::uint16_t>(port);
  server_options.max_connections = max_connections;
  server_options.allow_shutdown = allow_shutdown;

  Outcome<std::unique_ptr<ipc::Server>> server = ipc::Server::Start(*registry.value(), server_options);
  if (!server.has_value()) {
    std::fprintf(stderr, "the server could not be started: %s\n", server.error().describe().c_str());
    return 1;
  }

  std::printf("READY %u\n", static_cast<unsigned>(server.value()->port()));
  std::fflush(stdout);

  const bool requested = server.value()->wait_for_shutdown_request();
  server.value()->Stop();
  registry.value()->Close();
  if (!quiet) {
    std::printf("STOPPED %s\n", requested ? "requested" : "forced");
    std::fflush(stdout);
  }
  return 0;
}
