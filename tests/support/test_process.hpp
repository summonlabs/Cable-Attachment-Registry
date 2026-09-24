// Cable Attachment Registry — child process helper for the multiprocess tests.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Threads are not a multiprocess proof. These helpers start real operating
// system processes, read their standard output through a pipe, and can kill
// one outright so recovery from an unclean stop is exercised for real.

#ifndef CABLE_REGISTRY_TESTS_SUPPORT_TEST_PROCESS_HPP
#define CABLE_REGISTRY_TESTS_SUPPORT_TEST_PROCESS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace crtest {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  /// Starts @p executable with @p arguments. Standard output is captured in a
  /// pipe; standard error is inherited.
  static ChildProcess spawn(const std::string& executable, const std::vector<std::string>& arguments);

  /// Blocks until a complete line is available on the child's standard output.
  /// Returns an empty string when the stream ended first.
  std::string read_line();

  /// Reads standard output until the stream ends.
  std::string read_to_end();

  [[nodiscard]] bool running() const noexcept;
  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

  /// Waits for the process to exit and returns its exit code.
  int wait();

  /// Terminates the process immediately, as a power cut would. No cleanup
  /// handler in the child runs.
  void kill();

  void close_stdout();

 private:
  void release() noexcept;

  void* handle_ = nullptr;
  void* stdout_read_ = nullptr;
  std::uint64_t pid_ = 0;
  std::string buffer_;
};

/// A port number the operating system reported as free, obtained by binding
/// and immediately releasing a socket.
std::uint16_t free_port();

/// Opens a raw loopback TCP connection, sends @p data verbatim, half closes
/// the sending side, and reads whatever comes back. Used to prove that the
/// server rejects a peer that does not speak the frame protocol.
bool raw_exchange(std::uint16_t port, const std::vector<std::byte>& data, std::vector<std::byte>& reply);

} // namespace crtest

#endif // CABLE_REGISTRY_TESTS_SUPPORT_TEST_PROCESS_HPP
