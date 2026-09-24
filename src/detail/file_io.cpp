// Cable Attachment Registry — portable file primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "detail/file_io.hpp"

#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cable_registry::detail {
namespace {

Error io_error(const char* operation, const std::filesystem::path& path) {
#if defined(_WIN32)
  const DWORD code = ::GetLastError();
  return Error(ErrorCode::IoFailure,
               std::string(operation) + " failed for " + path.string() + " (windows error " +
                   std::to_string(static_cast<unsigned long>(code)) + ")");
#else
  return Error(ErrorCode::IoFailure,
               std::string(operation) + " failed for " + path.string() + ": " + std::strerror(errno));
#endif
}

} // namespace

FileHandle::~FileHandle() {
  Close();
}

FileHandle::FileHandle(FileHandle&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool FileHandle::valid() const noexcept {
#if defined(_WIN32)
  return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
#else
  return handle_ != nullptr && reinterpret_cast<std::intptr_t>(handle_) >= 0;
#endif
}

Outcome<FileHandle> FileHandle::Open(const std::filesystem::path& path, bool read, bool write, bool create) {
  if (!read && !write) {
    return make_error<FileHandle>(ErrorCode::InvalidArgument, "a file must be opened for reading, writing or both");
  }
  if (!create && !file_exists(path)) {
    return make_error<FileHandle>(ErrorCode::NotFound, "file does not exist: " + path.string());
  }

  FileHandle handle;
#if defined(_WIN32)
  DWORD access = 0;
  if (read) {
    access |= GENERIC_READ;
  }
  if (write) {
    access |= GENERIC_WRITE;
  }
  const DWORD disposition = create ? OPEN_ALWAYS : OPEN_EXISTING;
  // FILE_SHARE_READ keeps the log readable by an operator's inspection tool
  // while the registry holds it, and FILE_SHARE_DELETE is what lets the store
  // atomically replace its own log during compaction: without it the rename
  // fails with a sharing violation because this handle still has the file
  // open. Writes stay exclusive to this handle.
  HANDLE raw = ::CreateFileW(path.wstring().c_str(),
                             access,
                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                             nullptr,
                             disposition,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    return make_error<FileHandle>(io_error("CreateFile", path));
  }
  handle.handle_ = raw;
#else
  int flags = 0;
  if (read && write) {
    flags = O_RDWR;
  } else if (write) {
    flags = O_WRONLY;
  } else {
    flags = O_RDONLY;
  }
  if (create) {
    flags |= O_CREAT;
  }
  const int descriptor = ::open(path.string().c_str(), flags, 0644);
  if (descriptor < 0) {
    return make_error<FileHandle>(io_error("open", path));
  }
  handle.handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor));
#endif
  return handle;
}

void FileHandle::Close() noexcept {
  if (!valid()) {
    handle_ = nullptr;
    return;
  }
#if defined(_WIN32)
  ::CloseHandle(static_cast<HANDLE>(handle_));
#else
  ::close(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)));
#endif
  handle_ = nullptr;
}

Outcome<void> FileHandle::WriteAt(std::uint64_t offset, std::span<const std::byte> data) {
  if (!valid()) {
    return make_error(ErrorCode::IoFailure, "write on a closed file");
  }
  std::size_t written = 0;
  while (written < data.size()) {
#if defined(_WIN32)
    OVERLAPPED overlapped = {};
    const std::uint64_t position = offset + written;
    overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
    overlapped.OffsetHigh = static_cast<DWORD>((position >> 32) & 0xFFFFFFFFull);
    const DWORD chunk = static_cast<DWORD>(
        (data.size() - written) > 0x10000000ull ? 0x10000000ull : (data.size() - written));
    DWORD transferred = 0;
    if (::WriteFile(static_cast<HANDLE>(handle_), data.data() + written, chunk, &transferred, &overlapped) == 0) {
      return make_error(io_error("WriteFile", {}));
    }
    if (transferred == 0) {
      return make_error(ErrorCode::IoFailure, "WriteFile reported a zero length write");
    }
    written += transferred;
#else
    const ssize_t chunk = ::pwrite(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)),
                                   data.data() + written,
                                   data.size() - written,
                                   static_cast<off_t>(offset + written));
    if (chunk < 0) {
      if (errno == EINTR) {
        continue;
      }
      return make_error(io_error("pwrite", {}));
    }
    if (chunk == 0) {
      return make_error(ErrorCode::IoFailure, "pwrite reported a zero length write");
    }
    written += static_cast<std::size_t>(chunk);
#endif
  }
  return Outcome<void>();
}

Outcome<void> FileHandle::ReadAt(std::uint64_t offset, std::span<std::byte> buffer, std::size_t& bytes_read) {
  if (!valid()) {
    return make_error(ErrorCode::IoFailure, "read on a closed file");
  }
  bytes_read = 0;
  while (bytes_read < buffer.size()) {
#if defined(_WIN32)
    OVERLAPPED overlapped = {};
    const std::uint64_t position = offset + bytes_read;
    overlapped.Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
    overlapped.OffsetHigh = static_cast<DWORD>((position >> 32) & 0xFFFFFFFFull);
    const DWORD chunk = static_cast<DWORD>(
        (buffer.size() - bytes_read) > 0x10000000ull ? 0x10000000ull : (buffer.size() - bytes_read));
    DWORD transferred = 0;
    if (::ReadFile(static_cast<HANDLE>(handle_), buffer.data() + bytes_read, chunk, &transferred, &overlapped) == 0) {
      return make_error(io_error("ReadFile", {}));
    }
    if (transferred == 0) {
      break; // End of file.
    }
    bytes_read += transferred;
#else
    const ssize_t chunk = ::pread(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)),
                                  buffer.data() + bytes_read,
                                  buffer.size() - bytes_read,
                                  static_cast<off_t>(offset + bytes_read));
    if (chunk < 0) {
      if (errno == EINTR) {
        continue;
      }
      return make_error(io_error("pread", {}));
    }
    if (chunk == 0) {
      break; // End of file.
    }
    bytes_read += static_cast<std::size_t>(chunk);
#endif
  }
  return Outcome<void>();
}

Outcome<void> FileHandle::Sync() {
  if (!valid()) {
    return make_error(ErrorCode::IoFailure, "flush on a closed file");
  }
#if defined(_WIN32)
  if (::FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
    return make_error(io_error("FlushFileBuffers", {}));
  }
#else
  if (::fsync(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_))) != 0) {
    return make_error(io_error("fsync", {}));
  }
#endif
  return Outcome<void>();
}

Outcome<void> FileHandle::Truncate(std::uint64_t size) {
  if (!valid()) {
    return make_error(ErrorCode::IoFailure, "truncate on a closed file");
  }
#if defined(_WIN32)
  // The read and write paths are positional, so moving the file pointer here
  // does not disturb them.
  HANDLE raw = static_cast<HANDLE>(handle_);
  LARGE_INTEGER distance = {};
  distance.QuadPart = static_cast<LONGLONG>(size);
  if (::SetFilePointerEx(raw, distance, nullptr, FILE_BEGIN) == 0) {
    return make_error(io_error("SetFilePointerEx", {}));
  }
  if (::SetEndOfFile(raw) == 0) {
    return make_error(io_error("SetEndOfFile", {}));
  }
#else
  if (::ftruncate(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)), static_cast<off_t>(size)) != 0) {
    return make_error(io_error("ftruncate", {}));
  }
#endif
  return Outcome<void>();
}

Outcome<std::uint64_t> FileHandle::Size() const {
  if (!valid()) {
    return make_error<std::uint64_t>(ErrorCode::IoFailure, "size on a closed file");
  }
#if defined(_WIN32)
  LARGE_INTEGER size = {};
  if (::GetFileSizeEx(static_cast<HANDLE>(handle_), &size) == 0) {
    return make_error<std::uint64_t>(io_error("GetFileSizeEx", {}));
  }
  return static_cast<std::uint64_t>(size.QuadPart);
#else
  struct stat info = {};
  if (::fstat(static_cast<int>(reinterpret_cast<std::intptr_t>(handle_)), &info) != 0) {
    return make_error<std::uint64_t>(io_error("fstat", {}));
  }
  return static_cast<std::uint64_t>(info.st_size);
#endif
}

Outcome<void> atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(_WIN32)
  if (::MoveFileExW(from.wstring().c_str(),
                    to.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return make_error(io_error("MoveFileEx", to));
  }
#else
  if (::rename(from.string().c_str(), to.string().c_str()) != 0) {
    return make_error(io_error("rename", to));
  }
#endif
  return Outcome<void>();
}

Outcome<void> remove_file(const std::filesystem::path& path) {
  std::error_code code;
  std::filesystem::remove(path, code);
  if (code && code != std::errc::no_such_file_or_directory) {
    return make_error(ErrorCode::IoFailure, "remove failed for " + path.string() + ": " + code.message());
  }
  return Outcome<void>();
}

Outcome<void> create_parent_directories(const std::filesystem::path& path) {
  const std::filesystem::path parent = path.parent_path();
  if (parent.empty()) {
    return Outcome<void>();
  }
  std::error_code code;
  std::filesystem::create_directories(parent, code);
  if (code && !std::filesystem::is_directory(parent)) {
    return make_error(ErrorCode::IoFailure,
                      "could not create " + parent.string() + ": " + code.message());
  }
  return Outcome<void>();
}

bool file_exists(const std::filesystem::path& path) {
  std::error_code code;
  return std::filesystem::is_regular_file(path, code) && !code;
}

} // namespace cable_registry::detail
