// Cable Attachment Registry — portable file primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal header. The store needs exactly four things from the platform:
// positional reads and writes, a real flush to stable storage, truncation, and
// an atomic replace. Everything else is built on top of those.

#ifndef CABLE_REGISTRY_DETAIL_FILE_IO_HPP
#define CABLE_REGISTRY_DETAIL_FILE_IO_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include "cable_registry/error.hpp"

namespace cable_registry::detail {

/// An open file. Move-only; closes on destruction.
class FileHandle {
 public:
  FileHandle() = default;
  ~FileHandle();
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  /// Opens @p path. @p create makes the file when it does not exist.
  static Outcome<FileHandle> Open(const std::filesystem::path& path, bool read, bool write, bool create);

  [[nodiscard]] bool valid() const noexcept;
  void Close() noexcept;

  /// Writes the whole buffer at @p offset.
  Outcome<void> WriteAt(std::uint64_t offset, std::span<const std::byte> data);
  /// Reads as much as the file has at @p offset, up to the buffer size.
  Outcome<void> ReadAt(std::uint64_t offset, std::span<std::byte> buffer, std::size_t& bytes_read);
  /// Flushes to stable storage.
  Outcome<void> Sync();
  Outcome<void> Truncate(std::uint64_t size);
  [[nodiscard]] Outcome<std::uint64_t> Size() const;

 private:
  void* handle_ = nullptr;
};

/// Renames @p from over @p to, replacing it. The replacement is atomic with
/// respect to a crash: a reader sees either the old or the new file.
Outcome<void> atomic_replace(const std::filesystem::path& from, const std::filesystem::path& to);

/// Removes a file. Succeeds when the file is already gone.
Outcome<void> remove_file(const std::filesystem::path& path);

/// Creates every missing parent directory of @p path.
Outcome<void> create_parent_directories(const std::filesystem::path& path);

[[nodiscard]] bool file_exists(const std::filesystem::path& path);

} // namespace cable_registry::detail

#endif // CABLE_REGISTRY_DETAIL_FILE_IO_HPP
