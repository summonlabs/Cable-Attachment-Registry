// Cable Attachment Registry — versioned, integrity-checked append-only store.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_PERSISTENCE_HPP
#define CABLE_REGISTRY_PERSISTENCE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "cable_registry/digest.hpp"
#include "cable_registry/error.hpp"
#include "cable_registry/export.hpp"

namespace cable_registry {

/// What a store may do when the tail of the log is damaged.
enum class RecoveryPolicy : std::uint8_t {
  /// Refuse to open on any damage. The default.
  Strict = 0,
  /// Discard a damaged tail and open the intact prefix. Damage anywhere other
  /// than the tail is still fatal.
  SalvagePrefix,
};

CABLE_REGISTRY_API const char* to_string(RecoveryPolicy value) noexcept;

/// The kind of record in the log.
enum class RecordKind : std::uint16_t {
  /// A source session was opened, with the descriptor and incarnation.
  SessionOpen = 1,
  /// A source session was closed by its publisher.
  SessionClose = 2,
  /// One accepted evidence record, with the receive timestamp the registry
  /// assigned when it was first accepted.
  Evidence = 3,
  /// A compacted image of the whole registry state. Always the first record of
  /// a rewritten log; later records apply on top of it.
  StateSnapshot = 4,
};

CABLE_REGISTRY_API const char* to_string(RecordKind value) noexcept;

/// Store policy. Every size is bounded and every bound is enforced before an
/// allocation happens.
struct StoreOptions {
  std::filesystem::path path;
  /// Largest single record payload accepted. A larger frame is a corruption.
  std::uint64_t max_record_bytes = 1u << 20;
  /// Log size that triggers a caller-driven compaction opportunity.
  std::uint64_t max_log_bytes = 256ull << 20;
  /// Records that may accumulate before a compaction opportunity is reported.
  std::uint32_t max_records_before_compaction = 200'000;
  /// Flush the file to stable storage before an append is acknowledged.
  bool fsync_on_commit = true;
  RecoveryPolicy recovery_policy = RecoveryPolicy::Strict;
};

/// What happened while the store was opened and validated.
enum class RecoveryIssue : std::uint8_t {
  /// A new empty store was created.
  Created = 0,
  /// The store opened cleanly with all records intact.
  CleanOpen,
  /// A damaged tail was discarded. Only reachable under SalvagePrefix or when
  /// the damage is an incomplete final record.
  TruncatedTail,
  /// A record in the middle of the log failed validation and the prefix was
  /// salvaged.
  CorruptRecordSalvaged,
  /// The file header failed its magic, version or checksum.
  BadHeader,
  /// A record checksum or the running chain digest did not match.
  ChainBreak,
  /// The file was written by an unsupported format version.
  UnsupportedVersion,
  /// I/O failed while reading the log.
  IoFailure,
};

CABLE_REGISTRY_API const char* to_string(RecoveryIssue value) noexcept;

/// The result of opening a store. Reported to the operator verbatim.
struct RecoveryReport {
  bool opened = false;
  bool created = false;
  bool salvaged = false;
  bool chain_intact = false;
  std::uint32_t format_version = 0;
  std::uint64_t records_loaded = 0;
  std::uint64_t records_rejected = 0;
  std::uint64_t bytes_loaded = 0;
  std::uint64_t bytes_discarded = 0;
  RecoveryIssue issue = RecoveryIssue::Created;
  std::uint64_t issue_offset = 0;
  std::string detail;

  friend bool operator==(const RecoveryReport&, const RecoveryReport&) noexcept = default;
};

/// One record handed to Rewrite().
struct StoreRecord {
  RecordKind kind = RecordKind::Evidence;
  std::vector<std::byte> payload;
};

/// An append-only log with per-record checksums and a running chain digest.
///
/// The chain makes a torn, reordered or edited log detectable as a unit, not
/// only record by record. It is an integrity check against accidental damage:
/// it is not a signature and does not authenticate the writer.
class CABLE_REGISTRY_API Store {
 public:
  /// Visitor for a replay pass. Returning false stops the scan without error.
  using RecordVisitor = std::function<bool(RecordKind kind, std::span<const std::byte> payload)>;

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  ~Store();

  /// Opens, validates and, when allowed, repairs the log. On failure the
  /// report explains exactly what was rejected and at which offset.
  static Outcome<std::unique_ptr<Store>> Open(const StoreOptions& options, RecoveryReport& report);

  /// Appends one record and, when configured, makes it durable before
  /// returning. A failed append is never reported as success.
  Outcome<void> Append(RecordKind kind, std::span<const std::byte> payload);

  /// Flushes buffered bytes to stable storage.
  Outcome<void> Sync();

  /// Replaces the log with @p records, written to a temporary sibling and
  /// atomically renamed over the log. A crash before the rename leaves the
  /// original log intact and a stray temporary that the next open removes.
  Outcome<void> Rewrite(std::span<const StoreRecord> records);

  /// Replays the records that survived validation. Must be called before the
  /// first Append.
  Outcome<void> VisitRecords(const RecordVisitor& visitor);

  /// Flushes and releases the file. Idempotent.
  void Close();

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return options_.path; }
  [[nodiscard]] const StoreOptions& options() const noexcept { return options_; }
  [[nodiscard]] std::uint64_t bytes_on_disk() const noexcept { return bytes_on_disk_; }
  /// Records appended by this process since the store was opened.
  [[nodiscard]] std::uint64_t records_written() const noexcept { return records_written_; }
  /// Records the log holds: the ones found at open plus the ones appended
  /// since, or the ones written by the most recent rewrite.
  [[nodiscard]] std::uint64_t records_in_log() const noexcept { return records_loaded_ + records_written_; }
  [[nodiscard]] std::uint64_t records_since_rewrite() const noexcept { return records_since_rewrite_; }
  [[nodiscard]] std::uint64_t rewrite_count() const noexcept { return rewrite_count_; }
  [[nodiscard]] const Digest256& chain_digest() const noexcept { return chain_; }
  /// True once the log has grown past the configured record or byte budget.
  /// Compaction is always the caller's decision.
  [[nodiscard]] bool should_compact() const noexcept;

 private:
  Store() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  StoreOptions options_{};
  Digest256 chain_{};
  std::uint64_t bytes_on_disk_ = 0;
  std::uint64_t records_written_ = 0;
  std::uint64_t records_loaded_ = 0;
  std::uint64_t records_since_rewrite_ = 0;
  std::uint64_t rewrite_count_ = 0;
  bool replayed_ = false;
  bool closed_ = false;
};

} // namespace cable_registry

#endif // CABLE_REGISTRY_PERSISTENCE_HPP
