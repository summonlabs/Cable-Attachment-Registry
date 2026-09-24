// Cable Attachment Registry — portable blocking stream socket.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ipc/socket.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cable_registry::detail {
namespace {

#if defined(_WIN32)
constexpr std::intptr_t kInvalidSocket = static_cast<std::intptr_t>(INVALID_SOCKET);
using NativeSocket = SOCKET;
#else
constexpr std::intptr_t kInvalidSocket = -1;
using NativeSocket = int;
#endif

NativeSocket native_of(std::intptr_t handle) noexcept {
#if defined(_WIN32)
  return static_cast<NativeSocket>(handle);
#else
  return static_cast<NativeSocket>(handle);
#endif
}

Error socket_error(const char* operation) {
#if defined(_WIN32)
  return Error(ErrorCode::IoFailure,
               std::string(operation) + " failed (winsock error " + std::to_string(::WSAGetLastError()) + ")");
#else
  return Error(ErrorCode::IoFailure, std::string(operation) + " failed: " + std::strerror(errno));
#endif
}

void set_option(NativeSocket socket, int level, int name, int value) noexcept {
  ::setsockopt(socket, level, name, reinterpret_cast<const char*>(&value), sizeof(value));
}

} // namespace

Outcome<void> initialise_sockets() {
#if defined(_WIN32)
  static std::once_flag once;
  static int result = 0;
  std::call_once(once, [] {
    WSADATA data = {};
    result = ::WSAStartup(MAKEWORD(2, 2), &data);
  });
  if (result != 0) {
    return make_error(ErrorCode::IoFailure, "WSAStartup failed with code " + std::to_string(result));
  }
#endif
  return Outcome<void>();
}

Socket::~Socket() {
  close();
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
  other.handle_ = kInvalidSocket;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidSocket;
  }
  return *this;
}

bool Socket::valid() const noexcept {
  return handle_ != kInvalidSocket;
}

void Socket::close() noexcept {
  if (!valid()) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(native_of(handle_));
#else
  ::close(native_of(handle_));
#endif
  handle_ = kInvalidSocket;
}

void Socket::shutdown() noexcept {
  if (!valid()) {
    return;
  }
#if defined(_WIN32)
  ::shutdown(native_of(handle_), SD_BOTH);
#else
  ::shutdown(native_of(handle_), SHUT_RDWR);
#endif
}

Outcome<Socket> Socket::connect(const std::string& host, std::uint16_t port) {
  Outcome<void> ready = initialise_sockets();
  if (!ready.has_value()) {
    return make_error<Socket>(ready.error());
  }

  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* resolved = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
    return make_error<Socket>(ErrorCode::IoFailure, "could not resolve " + host + ":" + service);
  }

  Socket result;
  for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
    NativeSocket raw = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (raw == static_cast<NativeSocket>(kInvalidSocket)) {
      continue;
    }
    if (::connect(raw, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      set_option(raw, IPPROTO_TCP, 1 /* TCP_NODELAY */, 1);
      result.handle_ = static_cast<std::intptr_t>(raw);
      break;
    }
#if defined(_WIN32)
    ::closesocket(raw);
#else
    ::close(raw);
#endif
  }
  ::freeaddrinfo(resolved);
  if (!result.valid()) {
    return make_error<Socket>(ErrorCode::IoFailure, "could not connect to " + host + ":" + service);
  }
  return result;
}

Outcome<Socket> Socket::listen_on(const std::string& address,
                                  std::uint16_t port,
                                  std::uint32_t backlog,
                                  std::uint16_t& bound_port) {
  Outcome<void> ready = initialise_sockets();
  if (!ready.has_value()) {
    return make_error<Socket>(ready.error());
  }

  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* resolved = nullptr;
  const std::string service = std::to_string(port);
  const char* node = address.empty() ? nullptr : address.c_str();
  if (::getaddrinfo(node, service.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
    return make_error<Socket>(ErrorCode::IoFailure, "could not resolve bind address " + address);
  }

  NativeSocket raw = ::socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
  if (raw == static_cast<NativeSocket>(kInvalidSocket)) {
    ::freeaddrinfo(resolved);
    return make_error<Socket>(socket_error("socket"));
  }
  set_option(raw, SOL_SOCKET, SO_REUSEADDR, 1);
  if (::bind(raw, resolved->ai_addr, static_cast<int>(resolved->ai_addrlen)) != 0) {
    const Error failure = socket_error("bind");
    ::freeaddrinfo(resolved);
#if defined(_WIN32)
    ::closesocket(raw);
#else
    ::close(raw);
#endif
    return make_error<Socket>(failure);
  }
  ::freeaddrinfo(resolved);

  if (::listen(raw, static_cast<int>(backlog)) != 0) {
    const Error failure = socket_error("listen");
#if defined(_WIN32)
    ::closesocket(raw);
#else
    ::close(raw);
#endif
    return make_error<Socket>(failure);
  }

  sockaddr_in bound = {};
  int bound_length = static_cast<int>(sizeof(bound));
  if (::getsockname(raw, reinterpret_cast<sockaddr*>(&bound), &bound_length) == 0) {
    bound_port = ntohs(bound.sin_port);
  }

  Socket result;
  result.handle_ = static_cast<std::intptr_t>(raw);
  return result;
}

Outcome<Socket> Socket::accept() {
  if (!valid()) {
    return make_error<Socket>(ErrorCode::Closed, "accept on a closed socket");
  }
  sockaddr_storage peer = {};
  int peer_length = static_cast<int>(sizeof(peer));
  NativeSocket raw = ::accept(native_of(handle_), reinterpret_cast<sockaddr*>(&peer), &peer_length);
  if (raw == static_cast<NativeSocket>(kInvalidSocket)) {
    return make_error<Socket>(socket_error("accept"));
  }
  set_option(raw, IPPROTO_TCP, 1 /* TCP_NODELAY */, 1);
  Socket result;
  result.handle_ = static_cast<std::intptr_t>(raw);
  return result;
}

Outcome<std::pair<Socket, Socket>> Socket::make_pair() {
  std::uint16_t port = 0;
  Outcome<Socket> listener = listen_on("127.0.0.1", 0, 1, port);
  if (!listener.has_value()) {
    return make_error<std::pair<Socket, Socket>>(listener.error());
  }
  Outcome<Socket> client = connect("127.0.0.1", port);
  if (!client.has_value()) {
    listener.value().close();
    return make_error<std::pair<Socket, Socket>>(client.error());
  }
  Outcome<Socket> server = listener.value().accept();
  listener.value().close();
  if (!server.has_value()) {
    client.value().close();
    return make_error<std::pair<Socket, Socket>>(server.error());
  }
  return std::make_pair(std::move(client).value(), std::move(server).value());
}

Outcome<int> Socket::wait_readable(const Socket& first, const Socket& second) {
  if (!first.valid() || !second.valid()) {
    return make_error<int>(ErrorCode::Closed, "wait_readable on a closed socket");
  }
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(native_of(first.handle_), &readable);
  FD_SET(native_of(second.handle_), &readable);
#if defined(_WIN32)
  const int ready = ::select(0, &readable, nullptr, nullptr, nullptr);
#else
  const int highest = std::max(native_of(first.handle_), native_of(second.handle_));
  const int ready = ::select(highest + 1, &readable, nullptr, nullptr, nullptr);
#endif
  if (ready < 0) {
    return make_error<int>(socket_error("select"));
  }
  if (FD_ISSET(native_of(first.handle_), &readable) != 0) {
    return 0;
  }
  return 1;
}

Outcome<void> Socket::wake() {
  const std::byte marker{1};
  return send_all(std::span<const std::byte>(&marker, 1));
}

Outcome<void> Socket::send_all(std::span<const std::byte> data) {
  if (!valid()) {
    return make_error(ErrorCode::Closed, "send on a closed socket");
  }
  std::size_t sent = 0;
  while (sent < data.size()) {
    const std::size_t remaining = data.size() - sent;
    const int chunk = static_cast<int>(remaining > 0x40000000u ? 0x40000000u : remaining);
    const int written = ::send(native_of(handle_),
                               reinterpret_cast<const char*>(data.data() + sent),
                               chunk,
                               0);
    if (written <= 0) {
      return make_error(socket_error("send"));
    }
    sent += static_cast<std::size_t>(written);
  }
  return Outcome<void>();
}

Outcome<void> Socket::receive_exact(std::span<std::byte> buffer) {
  if (!valid()) {
    return make_error(ErrorCode::Closed, "receive on a closed socket");
  }
  std::size_t received = 0;
  while (received < buffer.size()) {
    const std::size_t remaining = buffer.size() - received;
    const int chunk = static_cast<int>(remaining > 0x40000000u ? 0x40000000u : remaining);
    const int got = ::recv(native_of(handle_), reinterpret_cast<char*>(buffer.data() + received), chunk, 0);
    if (got == 0) {
      return make_error(ErrorCode::Closed, "the peer closed the connection");
    }
    if (got < 0) {
      return make_error(socket_error("recv"));
    }
    received += static_cast<std::size_t>(got);
  }
  return Outcome<void>();
}

Outcome<bool> Socket::wait_ready(const Socket& socket, bool for_read, int timeout_ms) {
  if (!socket.valid()) {
    return make_error<bool>(ErrorCode::Closed, "wait on a closed socket");
  }
  fd_set readable;
  fd_set writable;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  if (for_read) {
    FD_SET(native_of(socket.handle_), &readable);
  } else {
    FD_SET(native_of(socket.handle_), &writable);
  }
  timeval timeout = {};
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
#if defined(_WIN32)
  const int ready = ::select(0, for_read ? &readable : nullptr, for_read ? nullptr : &writable, nullptr, &timeout);
#else
  const int highest = native_of(socket.handle_);
  const int ready =
      ::select(highest + 1, for_read ? &readable : nullptr, for_read ? nullptr : &writable, nullptr, &timeout);
#endif
  if (ready < 0) {
    return make_error<bool>(socket_error("select"));
  }
  return ready > 0;
}

Outcome<void> Socket::send_all(std::span<const std::byte> data, const std::atomic<bool>& stop, int poll_ms) {
  if (!valid()) {
    return make_error(ErrorCode::Closed, "send on a closed socket");
  }
  std::size_t sent = 0;
  while (sent < data.size()) {
    if (stop.load()) {
      return make_error(ErrorCode::Closed, "the server is stopping");
    }
    const Outcome<bool> ready = wait_ready(*this, false, poll_ms);
    if (!ready.has_value()) {
      return make_error(ready.error());
    }
    if (!ready.value()) {
      continue;
    }
    const std::size_t remaining = data.size() - sent;
    const int chunk = static_cast<int>(remaining > 0x40000000u ? 0x40000000u : remaining);
    const int written = ::send(native_of(handle_), reinterpret_cast<const char*>(data.data() + sent), chunk, 0);
    if (written <= 0) {
      return make_error(socket_error("send"));
    }
    sent += static_cast<std::size_t>(written);
  }
  return Outcome<void>();
}

Outcome<void> Socket::receive_exact(std::span<std::byte> buffer, const std::atomic<bool>& stop, int poll_ms) {
  if (!valid()) {
    return make_error(ErrorCode::Closed, "receive on a closed socket");
  }
  std::size_t received = 0;
  while (received < buffer.size()) {
    if (stop.load()) {
      return make_error(ErrorCode::Closed, "the server is stopping");
    }
    const Outcome<bool> ready = wait_ready(*this, true, poll_ms);
    if (!ready.has_value()) {
      return make_error(ready.error());
    }
    if (!ready.value()) {
      continue;
    }
    const std::size_t remaining = buffer.size() - received;
    const int chunk = static_cast<int>(remaining > 0x40000000u ? 0x40000000u : remaining);
    const int got = ::recv(native_of(handle_), reinterpret_cast<char*>(buffer.data() + received), chunk, 0);
    if (got == 0) {
      return make_error(ErrorCode::Closed, "the peer closed the connection");
    }
    if (got < 0) {
      return make_error(socket_error("recv"));
    }
    received += static_cast<std::size_t>(got);
  }
  return Outcome<void>();
}

} // namespace cable_registry::detail
