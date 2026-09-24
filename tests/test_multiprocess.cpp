// Cable Attachment Registry — independent process proof.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Threads are not a multiprocess proof. Every case here starts real operating
// system processes: a registry daemon, publication clients, and query clients.
// Processes are killed outright to prove that acknowledged evidence survives an
// unclean stop, and restarted to prove that a fresh incarnation fences what the
// previous one asserted.

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "cable_registry/cable_attachment_registry.hpp"
#include "registry_fixture.hpp"
#include "test_harness.hpp"
#include "test_process.hpp"

using namespace cable_registry;
using namespace crtest;

namespace {

constexpr const char* kTimeBase = "1700000000000000000";

std::string daemon_path() {
  const std::string& value = option("daemon");
  if (value.empty()) {
    ::crtest::fail(__FILE__, __LINE__, "the --daemon option was not supplied");
  }
  return value;
}

std::string client_path() {
  const std::string& value = option("cli");
  if (value.empty()) {
    ::crtest::fail(__FILE__, __LINE__, "the --cli option was not supplied");
  }
  return value;
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

std::string read_text(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

std::string hex_of(std::uint64_t value) {
  const Id128 raw{value, value ^ 0x5A5A5A5A5A5A5A5Aull};
  return to_hex(raw);
}

std::string object_hex(std::uint64_t lo) {
  return ObjectId::from_parts(0x0B1EC70000000000ull, lo).to_hex();
}

struct Daemon {
  ChildProcess process;
  std::uint16_t port = 0;
  [[nodiscard]] std::string endpoint() const { return "127.0.0.1:" + std::to_string(port); }
};

Daemon start_daemon(const std::vector<std::string>& arguments) {
  Daemon daemon;
  daemon.process = ChildProcess::spawn(daemon_path(), arguments);
  if (!daemon.process.valid()) {
    ::crtest::fail(__FILE__, __LINE__, "the daemon process could not be started");
  }
  // A blocking read: the daemon either announces its port or exits, and an
  // exit closes the pipe. There is no polling loop and no deadline.
  while (true) {
    const std::string line = daemon.process.read_line();
    if (line.empty()) {
      ::crtest::fail(__FILE__, __LINE__, "the daemon exited before reporting readiness");
    }
    if (line.rfind("READY ", 0) == 0) {
      daemon.port = static_cast<std::uint16_t>(std::stoul(line.substr(6)));
      break;
    }
  }
  return daemon;
}

int run_client(const std::vector<std::string>& arguments, std::string* output) {
  ChildProcess child = ChildProcess::spawn(client_path(), arguments);
  if (!child.valid()) {
    ::crtest::fail(__FILE__, __LINE__, "the client process could not be started");
  }
  const std::string text = child.read_to_end();
  const int code = child.wait();
  if (output != nullptr) {
    *output = text;
  }
  return code;
}

std::string script_for(const std::string& source_hex,
                       const std::string& source_name,
                       std::uint64_t object_base,
                       std::uint64_t endpoint_hex,
                       std::uint32_t first_port,
                       const std::string& incarnation) {
  std::string text;
  text += "source " + source_hex + " " + source_name + " Observed 7 " + incarnation + "\n";
  for (std::uint64_t index = 0; index < 2; ++index) {
    text += "object " + object_hex(object_base + index) + " L-" + source_name + "-" +
            std::to_string(index) + " 1\n";
  }
  text += "endpoint " + hex_of(endpoint_hex) + " sw-" + source_name + " 8\n";
  for (std::uint64_t index = 0; index < 2; ++index) {
    text += "attach " + object_hex(object_base + index) + " 0 " + hex_of(endpoint_hex) + " " +
            std::to_string(first_port + index) + "\n";
  }
  return text;
}

std::string digest_of(const std::string& text) {
  const std::string key = "\"graphDigest\":\"";
  const std::size_t start = text.find(key);
  if (start == std::string::npos) {
    return std::string();
  }
  const std::size_t begin = start + key.size();
  const std::size_t end = text.find('"', begin);
  if (end == std::string::npos) {
    return std::string();
  }
  return text.substr(begin, end - begin);
}

} // namespace

CR_TEST_CASE(multiprocess, independent_publishers_reach_the_same_graph_in_any_order) {
  TempDir directory("multiprocess-order");
  const std::string store_a = directory.file("a.log");
  const std::string store_b = directory.file("b.log");
  const std::string first = directory.file("first.script");
  const std::string second = directory.file("second.script");
  write_text(first, script_for(hex_of(0x1001), "alpha", 4100, 0x2001, 0, "1"));
  write_text(second, script_for(hex_of(0x1002), "beta", 4200, 0x2002, 4, "1"));

  // Registry A: the two publishers run one after the other.
  Daemon daemon_a = start_daemon({"--store", store_a, "--port", "0", "--quiet"});
  std::string output;
  CR_CHECK_EQ(run_client({"--endpoint", daemon_a.endpoint(), "--time-base", kTimeBase, "--quiet", "script",
                          first},
                         &output),
              0);
  CR_CHECK_EQ(run_client({"--endpoint", daemon_a.endpoint(), "--time-base", kTimeBase, "--quiet", "script",
                          second},
                         &output),
              0);
  CR_CHECK_EQ(run_client({"--endpoint", daemon_a.endpoint(), "digest"}, &output), 0);
  const std::string digest_a = digest_of(output);
  CR_CHECK(!digest_a.empty());
  daemon_a.process.kill();
  daemon_a.process.wait();

  // Registry B: the same two publishers run at the same time, from two
  // independent processes, against one daemon.
  Daemon daemon_b = start_daemon({"--store", store_b, "--port", "0", "--quiet"});
  ChildProcess publisher_one =
      ChildProcess::spawn(client_path(),
                          {"--endpoint", daemon_b.endpoint(), "--time-base", kTimeBase, "--quiet", "script",
                           first});
  ChildProcess publisher_two =
      ChildProcess::spawn(client_path(),
                          {"--endpoint", daemon_b.endpoint(), "--time-base", kTimeBase, "--quiet", "script",
                           second});
  const std::string one_output = publisher_one.read_to_end();
  const std::string two_output = publisher_two.read_to_end();
  CR_CHECK_EQ(publisher_one.wait(), 0);
  CR_CHECK_EQ(publisher_two.wait(), 0);
  CR_CHECK(one_output.find("\"refused\":0") != std::string::npos);
  CR_CHECK(two_output.find("\"refused\":0") != std::string::npos);
  CR_CHECK_EQ(run_client({"--endpoint", daemon_b.endpoint(), "digest"}, &output), 0);
  const std::string digest_b = digest_of(output);
  CR_CHECK(!digest_b.empty());

  CR_CHECK_MSG(digest_a == digest_b,
               "concurrent publication in independent processes produced a different graph");
  daemon_b.process.kill();
  daemon_b.process.wait();
}

CR_TEST_CASE(multiprocess, a_separate_query_process_sees_the_published_topology) {
  TempDir directory("multiprocess-query");
  const std::string store = directory.file("registry.log");
  const std::string script = directory.file("publish.script");
  const std::string endpoint_hex = hex_of(0x2003);
  write_text(script, script_for(hex_of(0x1003), "gamma", 4300, 0x2003, 0, "1"));

  Daemon daemon = start_daemon({"--store", store, "--port", "0", "--quiet"});
  std::string output;
  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "--time-base", kTimeBase, "--quiet", "script",
                          script},
                         &output),
              0);
  CR_CHECK(output.find("\"refused\":0") != std::string::npos);

  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "query-port", endpoint_hex, "0"}, &output), 0);
  CR_CHECK(output.find("\"Attached\"") != std::string::npos);
  CR_CHECK(output.find(object_hex(4300)) != std::string::npos);

  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "stats"}, &output), 0);
  CR_CHECK(output.find("\"objects\": 2") != std::string::npos);
  CR_CHECK(output.find("\"liveSessions\": 1") != std::string::npos);

  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "query-port", endpoint_hex, "7"}, &output), 0);
  CR_CHECK(output.find("\"Unknown\"") != std::string::npos);

  // A query for something that does not exist is a typed failure.
  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "query-object", object_hex(9999)}, &output), 1);

  daemon.process.kill();
  daemon.process.wait();
}

CR_TEST_CASE(multiprocess, a_hard_kill_leaves_acknowledged_evidence_durable) {
  TempDir directory("multiprocess-kill");
  const std::string store = directory.file("registry.log");
  const std::string script = directory.file("publish.script");
  const std::string endpoint_hex = hex_of(0x2004);
  write_text(script, script_for(hex_of(0x1004), "delta", 4400, 0x2004, 0, "1"));

  Daemon first = start_daemon({"--store", store, "--port", "0", "--quiet"});
  std::string output;
  CR_CHECK_EQ(run_client({"--endpoint", first.endpoint(), "--time-base", kTimeBase, "--quiet", "script",
                          script},
                         &output),
              0);
  CR_CHECK_EQ(run_client({"--endpoint", first.endpoint(), "digest"}, &output), 0);
  const std::string before = digest_of(output);
  CR_CHECK(!before.empty());

  // Terminate the daemon outright: no destructor, no flush, no cleanup.
  first.process.kill();
  first.process.wait();

  Daemon second = start_daemon({"--store", store, "--port", "0", "--quiet"});
  CR_CHECK_EQ(run_client({"--endpoint", second.endpoint(), "digest"}, &output), 0);
  CR_CHECK_MSG(digest_of(output) == before, "the graph changed across a hard kill");
  CR_CHECK_EQ(run_client({"--endpoint", second.endpoint(), "query-port", endpoint_hex, "0"}, &output), 0);
  CR_CHECK(output.find("\"Attached\"") != std::string::npos);
  // Recovered evidence is reported, and is explicitly not fresh.
  CR_CHECK(output.find("\"Unvalidated\"") != std::string::npos);
  CR_CHECK_EQ(run_client({"--endpoint", second.endpoint(), "query-port", endpoint_hex, "0", "--live-only"},
                         &output),
              0);
  CR_CHECK(output.find("\"Unknown\"") != std::string::npos);
  second.process.kill();
  second.process.wait();
}

CR_TEST_CASE(multiprocess, a_restarted_publisher_fences_its_previous_incarnation) {
  TempDir directory("multiprocess-fence");
  const std::string store = directory.file("registry.log");
  const std::string endpoint_hex = hex_of(0x2005);
  const std::string source_hex = hex_of(0x1005);
  const std::string first_script = directory.file("first.script");
  const std::string restart_script = directory.file("restart.script");
  const std::string republish_script = directory.file("republish.script");
  write_text(first_script, script_for(source_hex, "epsilon", 4500, 0x2005, 0, "1"));
  write_text(restart_script, "source " + source_hex + " epsilon Observed 7 2\n");
  std::string republish = "source " + source_hex + " epsilon Observed 7 2\n";
  republish += "attach " + object_hex(4500) + " 0 " + endpoint_hex + " 0\n";
  republish += "attach " + object_hex(4501) + " 0 " + endpoint_hex + " 1\n";
  write_text(republish_script, republish);

  Daemon daemon = start_daemon({"--store", store, "--port", "0", "--quiet"});
  std::string output;
  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "--time-base", kTimeBase, "--quiet", "script",
                          first_script},
                         &output),
              0);
  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "query-port", endpoint_hex, "0"}, &output), 0);
  CR_CHECK(output.find("\"Attached\"") != std::string::npos);

  // A new publisher process for the same source presents a higher incarnation.
  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "--time-base", kTimeBase, "--quiet", "script",
                          restart_script},
                         &output),
              0);
  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "query-port", endpoint_hex, "0"}, &output), 0);
  CR_CHECK_MSG(output.find("\"Unknown\"") != std::string::npos,
               "evidence from the fenced incarnation is still binding");

  // Re-asserting under the new incarnation restores it.
  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "--time-base", kTimeBase, "--quiet", "script",
                          republish_script},
                         &output),
              0);
  CR_CHECK(output.find("\"refused\":0") != std::string::npos);
  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "query-port", endpoint_hex, "0"}, &output), 0);
  CR_CHECK(output.find("\"Attached\"") != std::string::npos);
  CR_CHECK(output.find("\"Validated\"") != std::string::npos);

  // The fenced incarnation cannot be reopened.
  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "--time-base", kTimeBase, "open-source", source_hex,
                          "epsilon", "Observed", "7", "1"},
                         &output),
              0);
  CR_CHECK(output.find("refused-fenced") != std::string::npos);

  daemon.process.kill();
  daemon.process.wait();
}

CR_TEST_CASE(multiprocess, a_damaged_store_is_refused_by_a_fresh_process) {
  TempDir directory("multiprocess-damaged");
  const std::string store = directory.file("registry.log");
  const std::string script = directory.file("publish.script");
  write_text(script, script_for(hex_of(0x1006), "zeta", 4600, 0x2006, 0, "1"));

  Daemon daemon = start_daemon({"--store", store, "--port", "0", "--quiet"});
  std::string output;
  CR_CHECK_EQ(run_client({"--endpoint", daemon.endpoint(), "--time-base", kTimeBase, "--quiet", "script",
                          script},
                         &output),
              0);
  daemon.process.kill();
  daemon.process.wait();

  std::string bytes = read_text(store);
  CR_CHECK(bytes.size() > 256);
  const std::size_t target = bytes.size() / 2;
  bytes[target] = static_cast<char>(static_cast<unsigned char>(bytes[target]) ^ 0x5Au);
  write_text(store, bytes);

  // Without the salvage policy the daemon refuses the store: it reports the
  // damage and exits non-zero instead of serving a partial graph.
  ChildProcess strict = ChildProcess::spawn(daemon_path(), {"--store", store, "--port", "0", "--quiet"});
  const std::string strict_output = strict.read_to_end();
  const int strict_code = strict.wait();
  CR_CHECK_MSG(strict_code != 0, "the daemon served a damaged store instead of refusing it");
  CR_CHECK(strict_output.find("READY") == std::string::npos);

  // With the salvage policy it opens the intact prefix and reports what it lost.
  Daemon salvaged = start_daemon({"--store", store, "--port", "0", "--salvage"});
  CR_CHECK(salvaged.port != 0);
  CR_CHECK_EQ(run_client({"--endpoint", salvaged.endpoint(), "stats"}, &output), 0);
  CR_CHECK(output.find("\"salvaged\": true") != std::string::npos);
  CR_CHECK(output.find("\"bytesDiscarded\": 0") == std::string::npos);
  salvaged.process.kill();
  salvaged.process.wait();
}
