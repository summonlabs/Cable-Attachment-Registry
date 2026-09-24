// Cable Attachment Registry — portable blocking stream socket.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal header. The publisher transport needs a real stream socket that
// carries a length prefix, survives partial reads and writes, and can be woken
// from another thread by a shutdown so a blocked worker never has to be killed.

#ifndef CABLE_REGISTRY_DETAIL_SOCKET_HPP
#define CABLE_REGISTRY_DETAIL_SOCKET_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "cable_registry/error.hpp"

namespace cable_registry::detail {

/// Initialises the platform socket layer once per process. Idempotent.
Outcome<void> initialise_sockets();

class Socket {
 public:
  Socket() = default;
  ~Socket();
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  /// Connects to @p host on @p port.
  static Outcome<Socket> connect(const std::string& host, std::uint16_t port);

  /// Binds and listens. @p bound_port receives the port actually bound, which
  /// is what an ephemeral request resolves to.
  static Outcome<Socket> listen_on(const std::string& address,
                                   std::uint16_t port,
                                   std::uint32_t backlog,
                                   std::uint16_t& bound_port);

  /// Accepts the next connection. Fails when the socket has been shut down.
  Outcome<Socket> accept();

  Outcome<void> send_all(std::span<const std::byte> data);
  Outcome<void> receive_exact(std::span<std::byte> buffer);

  /// Interruptible variants. They wait for the socket to become ready in
  /// bounded slices and check @p stop between slices, so a cooperating thread
  /// stops without anyone having to cancel a blocked call from another thread —
  /// which is not portable and, on Windows, does not work for a blocked
  /// receive at all. @p poll_ms is an I/O multiplexing interval, not a session
  /// deadline: a transfer that keeps making progress is never cut short.
  Outcome<void> send_all(std::span<const std::byte> data, const std::atomic<bool>& stop, int poll_ms);
  Outcome<void> receive_exact(std::span<std::byte> buffer, const std::atomic<bool>& stop, int poll_ms);

  /// Waits until @p socket is readable (or writable) or @p timeout_ms elapses.
  /// Returns true when it is ready, false on timeout.
  static Outcome<bool> wait_ready(const Socket& socket, bool for_read, int timeout_ms);

  /// Creates a connected pair of loopback sockets. One end is used to wake a
  /// thread blocked in accept(), which no portable call can otherwise cancel.
  static Outcome<std::pair<Socket, Socket>> make_pair();

  /// Blocks until @p first or @p second has a pending read (or a pending
  /// connection, for a listening socket). Returns 0 for @p first and 1 for
  /// @p second. Fails when the wait itself fails.
  static Outcome<int> wait_readable(const Socket& first, const Socket& second);

  /// Sends a single wakeup byte. Used with make_pair().
  Outcome<void> wake();

  /// Wakes every blocked call on this socket in both directions. Safe to call
  /// from another thread.
  void shutdown() noexcept;
  void close() noexcept;
  [[nodiscard]] bool valid() const noexcept;

 private:
  std::intptr_t handle_ = -1;
};

} // namespace cable_registry::detail

#endif // CABLE_REGISTRY_DETAIL_SOCKET_HPP
