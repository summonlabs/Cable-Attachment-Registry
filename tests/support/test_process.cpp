// Cable Attachment Registry — child process helper implementation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_process.hpp"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace crtest {
namespace {

#if defined(_WIN32)
std::string quote_argument(const std::string& argument) {
  std::string quoted = "\"";
  for (const char character : argument) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted += character;
    }
  }
  quoted += '"';
  return quoted;
}
#endif

} // namespace

ChildProcess::~ChildProcess() {
  if (handle_ != nullptr) {
    kill();
    wait();
  }
  release();
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), stdout_read_(other.stdout_read_), pid_(other.pid_), buffer_(std::move(other.buffer_)) {
  other.handle_ = nullptr;
  other.stdout_read_ = nullptr;
  other.pid_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (handle_ != nullptr) {
      kill();
      wait();
    }
    release();
    handle_ = other.handle_;
    stdout_read_ = other.stdout_read_;
    pid_ = other.pid_;
    buffer_ = std::move(other.buffer_);
    other.handle_ = nullptr;
    other.stdout_read_ = nullptr;
    other.pid_ = 0;
  }
  return *this;
}

void ChildProcess::release() noexcept {
#if defined(_WIN32)
  if (stdout_read_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(stdout_read_));
    stdout_read_ = nullptr;
  }
  if (handle_ != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (stdout_read_ != nullptr) {
    ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)));
    stdout_read_ = nullptr;
  }
  handle_ = nullptr;
#endif
}

ChildProcess ChildProcess::spawn(const std::string& executable, const std::vector<std::string>& arguments) {
  ChildProcess child;
#if defined(_WIN32)
  SECURITY_ATTRIBUTES attributes = {};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;

  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (::CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
    return child;
  }
  ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

  std::string command = quote_argument(executable);
  for (const std::string& argument : arguments) {
    command += ' ';
    command += quote_argument(argument);
  }

  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_end;
  startup.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION information = {};
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  const BOOL started = ::CreateProcessA(nullptr,
                                        mutable_command.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        CREATE_NO_WINDOW,
                                        nullptr,
                                        nullptr,
                                        &startup,
                                        &information);
  ::CloseHandle(write_end);
  if (started == 0) {
    ::CloseHandle(read_end);
    return child;
  }
  ::CloseHandle(information.hThread);
  child.handle_ = information.hProcess;
  child.stdout_read_ = read_end;
  child.pid_ = information.dwProcessId;
#else
  int descriptors[2] = {-1, -1};
  if (::pipe(descriptors) != 0) {
    return child;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    return child;
  }
  if (pid == 0) {
    ::close(descriptors[0]);
    ::dup2(descriptors[1], STDOUT_FILENO);
    ::close(descriptors[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(descriptors[1]);
  child.stdout_read_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptors[0]));
  child.pid_ = static_cast<std::uint64_t>(pid);
  child.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(1));
#endif
  return child;
}

std::string ChildProcess::read_line() {
  if (stdout_read_ == nullptr) {
    return std::string();
  }
  while (true) {
    const std::size_t newline = buffer_.find('\n');
    if (newline != std::string::npos) {
      std::string line = buffer_.substr(0, newline);
      buffer_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      return line;
    }
    char chunk[256] = {};
#if defined(_WIN32)
    DWORD got = 0;
    if (::ReadFile(static_cast<HANDLE>(stdout_read_), chunk, sizeof(chunk), &got, nullptr) == 0 || got == 0) {
      break;
    }
#else
    const ssize_t got = ::read(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)), chunk, sizeof(chunk));
    if (got <= 0) {
      break;
    }
#endif
    buffer_.append(chunk, static_cast<std::size_t>(got));
  }
  std::string line = std::move(buffer_);
  buffer_.clear();
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  return line;
}

std::string ChildProcess::read_to_end() {
  std::string collected = buffer_;
  buffer_.clear();
  if (stdout_read_ == nullptr) {
    return collected;
  }
  char chunk[512] = {};
  while (true) {
#if defined(_WIN32)
    DWORD got = 0;
    if (::ReadFile(static_cast<HANDLE>(stdout_read_), chunk, sizeof(chunk), &got, nullptr) == 0 || got == 0) {
      break;
    }
#else
    const ssize_t got = ::read(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)), chunk, sizeof(chunk));
    if (got <= 0) {
      break;
    }
#endif
    collected.append(chunk, static_cast<std::size_t>(got));
  }
  return collected;
}

void ChildProcess::close_stdout() {
  if (stdout_read_ == nullptr) {
    return;
  }
#if defined(_WIN32)
  ::CloseHandle(static_cast<HANDLE>(stdout_read_));
#else
  ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(stdout_read_)));
#endif
  stdout_read_ = nullptr;
}

bool ChildProcess::running() const noexcept {
  if (handle_ == nullptr) {
    return false;
  }
#if defined(_WIN32)
  return ::WaitForSingleObject(static_cast<HANDLE>(handle_), 0) == WAIT_TIMEOUT;
#else
  int status = 0;
  const pid_t result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  return result == 0;
#endif
}

int ChildProcess::wait() {
  if (handle_ == nullptr) {
    return -1;
  }
#if defined(_WIN32)
  ::WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  DWORD code = 0;
  ::GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
  return static_cast<int>(code);
#else
  int status = 0;
  if (::waitpid(static_cast<pid_t>(pid_), &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
}

void ChildProcess::kill() {
  if (handle_ == nullptr) {
    return;
  }
#if defined(_WIN32)
  ::TerminateProcess(static_cast<HANDLE>(handle_), 3);
#else
  ::kill(static_cast<pid_t>(pid_), SIGKILL);
#endif
}

bool raw_exchange(std::uint16_t port, const std::vector<std::byte>& data, std::vector<std::byte>& reply) {
  reply.clear();
#if defined(_WIN32)
  WSADATA wsa = {};
  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return false;
  }
#endif
  const int descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (descriptor < 0) {
    return false;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(port);
  if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
#if defined(_WIN32)
    ::closesocket(descriptor);
#else
    ::close(descriptor);
#endif
    return false;
  }
  std::size_t sent = 0;
  while (sent < data.size()) {
    const int written = ::send(descriptor,
                               reinterpret_cast<const char*>(data.data() + sent),
                               static_cast<int>(data.size() - sent),
                               0);
    if (written <= 0) {
      break;
    }
    sent += static_cast<std::size_t>(written);
  }
#if defined(_WIN32)
  ::shutdown(descriptor, SD_SEND);
#else
  ::shutdown(descriptor, SHUT_WR);
#endif
  char buffer[512] = {};
  while (true) {
    const int got = ::recv(descriptor, buffer, sizeof(buffer), 0);
    if (got <= 0) {
      break;
    }
    for (int index = 0; index < got; ++index) {
      reply.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[index])));
    }
  }
#if defined(_WIN32)
  ::closesocket(descriptor);
#else
  ::close(descriptor);
#endif
  return true;
}

std::uint16_t free_port() {
#if defined(_WIN32)
  WSADATA data = {};
  if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
    return 0;
  }
#endif
  const int descriptor = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
  if (descriptor < 0) {
    return 0;
  }
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
#if defined(_WIN32)
    ::closesocket(descriptor);
#else
    ::close(descriptor);
#endif
    return 0;
  }
  socklen_t length = sizeof(address);
  std::uint16_t port = 0;
  if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
    port = ntohs(address.sin_port);
  }
#if defined(_WIN32)
  ::closesocket(descriptor);
#else
  ::close(descriptor);
#endif
  return port;
}

} // namespace crtest
