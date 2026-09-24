// Cable Attachment Registry — versioned, integrity-checked append-only store.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Log layout
// ----------
//   header : magic "CABLEARG" (8 bytes), format version (u32), flags (u32),
//            header bytes (u32), header crc32c (u32)
//   record : frame bytes (u32), kind (u16), flags (u16), sequence (u64),
//            payload bytes (u32), payload crc32c (u32), payload, chain (32),
//            frame crc32c (u32)
//
//   chain_n = SHA-256(chain_{n-1} || kind || flags || sequence || payload)
//
// The frame checksum catches a torn or bit-rotted record; the chain catches a
// removed, reordered or spliced one. Neither is a signature: the store detects
// accidental damage, it does not authenticate the writer.
//
// Recovery happens once, in Open(): the whole log is scanned and validated,
// the outcome is reported, and a torn final record is truncated away. A
// damaged record anywhere else is fatal under Strict and salvaged as a prefix
// under SalvagePrefix. VisitRecords() then replays the validated prefix.

#include "cable_registry/persistence.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <utility>

#include "cable_registry/codec.hpp"
#include "cable_registry/digest.hpp"
#include "cable_registry/version.hpp"
#include "detail/file_io.hpp"

namespace cable_registry {
namespace {

constexpr char kStoreMagic[8] = {'C', 'A', 'B', 'L', 'E', 'A', 'R', 'G'};
constexpr std::uint32_t kHeaderBytes = 8 + 4 + 4 + 4 + 4;
constexpr std::uint32_t kRecordHeaderBytes = 4 + 2 + 2 + 8 + 4 + 4;
constexpr std::uint32_t kChainBytes = 32;
constexpr std::uint32_t kRecordTrailerBytes = kChainBytes + 4;
constexpr std::uint32_t kMinRecordBytes = kRecordHeaderBytes + kRecordTrailerBytes;
constexpr std::uint16_t kRecordFlagsNone = 0;

void store_u16(std::span<std::byte> out, std::size_t offset, std::uint16_t value) noexcept {
  for (int index = 0; index < 2; ++index) {
    out[offset + static_cast<std::size_t>(index)] = static_cast<std::byte>((value >> (index * 8)) & 0xFFu);
  }
}

void store_u32(std::span<std::byte> out, std::size_t offset, std::uint32_t value) noexcept {
  for (int index = 0; index < 4; ++index) {
    out[offset + static_cast<std::size_t>(index)] = static_cast<std::byte>((value >> (index * 8)) & 0xFFu);
  }
}

void store_u64(std::span<std::byte> out, std::size_t offset, std::uint64_t value) noexcept {
  for (int index = 0; index < 8; ++index) {
    out[offset + static_cast<std::size_t>(index)] = static_cast<std::byte>((value >> (index * 8)) & 0xFFu);
  }
}

std::uint16_t load_u16(std::span<const std::byte> in, std::size_t offset) noexcept {
  std::uint16_t value = 0;
  for (int index = 0; index < 2; ++index) {
    value |= static_cast<std::uint16_t>(in[offset + static_cast<std::size_t>(index)]) << (index * 8);
  }
  return value;
}

std::uint32_t load_u32(std::span<const std::byte> in, std::size_t offset) noexcept {
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(in[offset + static_cast<std::size_t>(index)]) << (index * 8);
  }
  return value;
}

std::uint64_t load_u64(std::span<const std::byte> in, std::size_t offset) noexcept {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(in[offset + static_cast<std::size_t>(index)]) << (index * 8);
  }
  return value;
}

bool valid_record_kind(RecordKind kind) noexcept {
  return kind == RecordKind::SessionOpen || kind == RecordKind::SessionClose || kind == RecordKind::Evidence ||
         kind == RecordKind::StateSnapshot;
}

/// The chain contribution of one record: everything from the kind field up to
/// the end of the payload.
Digest256 chain_step(const Digest256& previous,
                     std::span<const std::byte> header_from_kind,
                     std::span<const std::byte> payload) noexcept {
  Sha256 hasher;
  hasher.update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(previous.bytes.data()),
                                           previous.bytes.size()));
  hasher.update(header_from_kind);
  hasher.update(payload);
  return hasher.finish();
}

void fill_header(std::span<std::byte> header) noexcept {
  std::memcpy(header.data(), kStoreMagic, sizeof(kStoreMagic));
  store_u32(header, 8, kStoreFormatVersion);
  store_u32(header, 12, 0);
  store_u32(header, 16, kHeaderBytes);
  store_u32(header, 20, crc32c(std::span<const std::byte>(header.data(), 20)));
}

/// Why a scan stopped.
struct ScanStop {
  bool stopped = false;
  bool tail_incomplete = false;
  RecoveryIssue issue = RecoveryIssue::CleanOpen;
  std::uint64_t offset = 0;
  std::string detail;
};

} // namespace

const char* to_string(RecoveryPolicy value) noexcept {
  switch (value) {
    case RecoveryPolicy::Strict:
      return "strict";
    case RecoveryPolicy::SalvagePrefix:
      return "salvage-prefix";
  }
  return "strict";
}

const char* to_string(RecordKind value) noexcept {
  switch (value) {
    case RecordKind::SessionOpen:
      return "session-open";
    case RecordKind::SessionClose:
      return "session-close";
    case RecordKind::Evidence:
      return "evidence";
    case RecordKind::StateSnapshot:
      return "state-snapshot";
  }
  return "unknown";
}

const char* to_string(RecoveryIssue value) noexcept {
  switch (value) {
    case RecoveryIssue::Created:
      return "created";
    case RecoveryIssue::CleanOpen:
      return "clean-open";
    case RecoveryIssue::TruncatedTail:
      return "truncated-tail";
    case RecoveryIssue::CorruptRecordSalvaged:
      return "corrupt-record-salvaged";
    case RecoveryIssue::BadHeader:
      return "bad-header";
    case RecoveryIssue::ChainBreak:
      return "chain-break";
    case RecoveryIssue::UnsupportedVersion:
      return "unsupported-version";
    case RecoveryIssue::IoFailure:
      return "io-failure";
  }
  return "unknown";
}

struct Store::Impl {
  detail::FileHandle file;
  std::uint64_t write_offset = 0;
  std::uint64_t valid_end = 0;
  std::uint64_t next_sequence = 1;
  std::vector<std::byte> scratch;
};

Store::~Store() {
  Close();
}

bool Store::should_compact() const noexcept {
  return records_since_rewrite_ >= options_.max_records_before_compaction ||
         bytes_on_disk_ >= options_.max_log_bytes;
}

Outcome<std::unique_ptr<Store>> Store::Open(const StoreOptions& options, RecoveryReport& report) {
  report = RecoveryReport{};
  report.format_version = kStoreFormatVersion;

  if (options.path.empty()) {
    return make_error<std::unique_ptr<Store>>(ErrorCode::InvalidArgument, "the store path is empty");
  }
  if (options.max_record_bytes < kMinRecordBytes) {
    return make_error<std::unique_ptr<Store>>(
        ErrorCode::InvalidArgument,
        "store max_record_bytes must be at least " + std::to_string(kMinRecordBytes));
  }
  if (options.max_log_bytes < (4u * kHeaderBytes)) {
    return make_error<std::unique_ptr<Store>>(ErrorCode::InvalidArgument,
                                              "store max_log_bytes is implausibly small");
  }

  Outcome<void> directories = detail::create_parent_directories(options.path);
  if (!directories.has_value()) {
    report.issue = RecoveryIssue::IoFailure;
    report.detail = directories.error().message();
    return make_error<std::unique_ptr<Store>>(directories.error());
  }

  const std::filesystem::path temporary = std::filesystem::path(options.path.string() + ".tmp");
  if (detail::file_exists(temporary)) {
    // A leftover rewrite target was never committed; the committed log is
    // still authoritative, so removing it is the conservative action.
    Outcome<void> removed = detail::remove_file(temporary);
    if (!removed.has_value()) {
      report.issue = RecoveryIssue::IoFailure;
      report.detail = removed.error().message();
      return make_error<std::unique_ptr<Store>>(removed.error());
    }
  }

  const bool existed = detail::file_exists(options.path);
  auto store = std::unique_ptr<Store>(new Store());
  store->options_ = options;
  store->impl_ = std::make_unique<Impl>();

  Outcome<detail::FileHandle> handle = detail::FileHandle::Open(options.path, true, true, true);
  if (!handle.has_value()) {
    report.issue = RecoveryIssue::IoFailure;
    report.detail = handle.error().message();
    return make_error<std::unique_ptr<Store>>(handle.error());
  }
  store->impl_->file = std::move(handle).value();

  Outcome<std::uint64_t> size = store->impl_->file.Size();
  if (!size.has_value()) {
    report.issue = RecoveryIssue::IoFailure;
    report.detail = size.error().message();
    return make_error<std::unique_ptr<Store>>(size.error());
  }
  const std::uint64_t file_size = size.value();
  report.opened = true;
  report.chain_intact = true;
  store->chain_ = sha256(std::string_view());

  if (!existed || file_size == 0) {
    std::array<std::byte, kHeaderBytes> header{};
    fill_header(header);
    Outcome<void> written = store->impl_->file.WriteAt(0, header);
    if (!written.has_value()) {
      return make_error<std::unique_ptr<Store>>(written.error());
    }
    Outcome<void> synced = store->impl_->file.Sync();
    if (!synced.has_value()) {
      return make_error<std::unique_ptr<Store>>(synced.error());
    }
    store->impl_->write_offset = kHeaderBytes;
    store->impl_->valid_end = kHeaderBytes;
    store->bytes_on_disk_ = kHeaderBytes;
    report.created = true;
    report.issue = RecoveryIssue::Created;
    report.detail = "created a new store";
    report.bytes_loaded = kHeaderBytes;
    return store;
  }

  if (file_size < kHeaderBytes) {
    report.opened = false;
    report.chain_intact = false;
    report.issue = RecoveryIssue::BadHeader;
    report.detail = "store is shorter than its header";
    return make_error<std::unique_ptr<Store>>(
        ErrorCode::TruncatedStore,
        "store is " + std::to_string(file_size) + " bytes, shorter than the " + std::to_string(kHeaderBytes) +
            " byte header");
  }

  std::array<std::byte, kHeaderBytes> header{};
  std::size_t header_read = 0;
  Outcome<void> read_header =
      store->impl_->file.ReadAt(0, std::span<std::byte>(header.data(), header.size()), header_read);
  if (!read_header.has_value()) {
    report.opened = false;
    report.issue = RecoveryIssue::IoFailure;
    report.detail = read_header.error().message();
    return make_error<std::unique_ptr<Store>>(read_header.error());
  }
  if (header_read != kHeaderBytes || std::memcmp(header.data(), kStoreMagic, sizeof(kStoreMagic)) != 0) {
    report.opened = false;
    report.chain_intact = false;
    report.issue = RecoveryIssue::BadHeader;
    report.detail = "store magic does not match";
    return make_error<std::unique_ptr<Store>>(ErrorCode::CorruptStore,
                                              "store magic does not match; this is not a cable registry store");
  }
  if (load_u32(header, 20) != crc32c(std::span<const std::byte>(header.data(), 20))) {
    report.opened = false;
    report.chain_intact = false;
    report.issue = RecoveryIssue::BadHeader;
    report.detail = "store header checksum does not match";
    return make_error<std::unique_ptr<Store>>(ErrorCode::CorruptStore, "store header checksum does not match");
  }
  const std::uint32_t format = load_u32(header, 8);
  report.format_version = format;
  if (format != kStoreFormatVersion) {
    report.opened = false;
    report.chain_intact = false;
    report.issue = RecoveryIssue::UnsupportedVersion;
    report.detail = "store format version " + std::to_string(format) + " is not supported";
    return make_error<std::unique_ptr<Store>>(ErrorCode::UnsupportedVersion, report.detail);
  }
  if (load_u32(header, 16) != kHeaderBytes) {
    report.opened = false;
    report.chain_intact = false;
    report.issue = RecoveryIssue::BadHeader;
    report.detail = "store header length does not match the format";
    return make_error<std::unique_ptr<Store>>(ErrorCode::CorruptStore, report.detail);
  }

  // Scan and validate the whole log.
  Digest256 chain = store->chain_;
  std::uint64_t offset = kHeaderBytes;
  std::uint64_t sequence = 1;
  std::uint64_t loaded = 0;
  ScanStop stop;
  std::vector<std::byte> scratch;
  std::array<std::byte, kRecordHeaderBytes> record_header{};

  while (offset < file_size) {
    const std::uint64_t remaining = file_size - offset;
    if (remaining < kRecordHeaderBytes) {
      stop = ScanStop{true, true, RecoveryIssue::TruncatedTail, offset, "trailing bytes shorter than a record header"};
      break;
    }
    std::size_t read = 0;
    Outcome<void> header_result =
        store->impl_->file.ReadAt(offset, std::span<std::byte>(record_header.data(), record_header.size()), read);
    if (!header_result.has_value()) {
      report.opened = false;
      report.issue = RecoveryIssue::IoFailure;
      report.detail = header_result.error().message();
      return make_error<std::unique_ptr<Store>>(header_result.error());
    }
    if (read != kRecordHeaderBytes) {
      stop = ScanStop{true, true, RecoveryIssue::TruncatedTail, offset, "record header is incomplete"};
      break;
    }

    const std::uint32_t frame_bytes = load_u32(record_header, 0);
    const auto kind = static_cast<RecordKind>(load_u16(record_header, 4));
    const std::uint16_t flags = load_u16(record_header, 6);
    const std::uint64_t record_sequence = load_u64(record_header, 8);
    const std::uint32_t payload_bytes = load_u32(record_header, 16);
    const std::uint32_t payload_crc = load_u32(record_header, 20);

    if (static_cast<std::uint64_t>(frame_bytes) + 4 > remaining) {
      stop = ScanStop{true, true, RecoveryIssue::TruncatedTail, offset, "record extends past the end of the store"};
      break;
    }
    if (frame_bytes < (kMinRecordBytes - 4)) {
      stop = ScanStop{true, false, RecoveryIssue::CorruptRecordSalvaged, offset, "record frame length is too small"};
      break;
    }
    if (flags != kRecordFlagsNone) {
      stop = ScanStop{true, false, RecoveryIssue::CorruptRecordSalvaged, offset, "record flags are not understood"};
      break;
    }
    if (!valid_record_kind(kind)) {
      stop = ScanStop{true, false, RecoveryIssue::CorruptRecordSalvaged, offset, "record kind is not understood"};
      break;
    }
    if (static_cast<std::uint64_t>(payload_bytes) + kRecordHeaderBytes + kRecordTrailerBytes !=
        static_cast<std::uint64_t>(frame_bytes) + 4) {
      stop = ScanStop{true, false, RecoveryIssue::CorruptRecordSalvaged, offset, "record payload length is inconsistent"};
      break;
    }
    if (payload_bytes > options.max_record_bytes) {
      stop = ScanStop{true, false, RecoveryIssue::CorruptRecordSalvaged, offset, "record payload exceeds the configured limit"};
      break;
    }
    if (record_sequence != sequence) {
      stop = ScanStop{true, false, RecoveryIssue::ChainBreak, offset, "record sequence is not contiguous"};
      break;
    }

    scratch.assign(static_cast<std::size_t>(frame_bytes) + 4, std::byte{0});
    std::size_t body_read = 0;
    Outcome<void> body_result =
        store->impl_->file.ReadAt(offset, std::span<std::byte>(scratch.data(), scratch.size()), body_read);
    if (!body_result.has_value()) {
      report.opened = false;
      report.issue = RecoveryIssue::IoFailure;
      report.detail = body_result.error().message();
      return make_error<std::unique_ptr<Store>>(body_result.error());
    }
    if (body_read != scratch.size()) {
      stop = ScanStop{true, true, RecoveryIssue::TruncatedTail, offset, "record body is incomplete"};
      break;
    }

    const std::size_t crc_offset = kRecordHeaderBytes + payload_bytes + kChainBytes;
    if (load_u32(std::span<const std::byte>(scratch.data(), scratch.size()), crc_offset) !=
        crc32c(std::span<const std::byte>(scratch.data(), crc_offset))) {
      stop = ScanStop{true, false, RecoveryIssue::CorruptRecordSalvaged, offset, "record checksum does not match"};
      break;
    }
    const std::span<const std::byte> payload(scratch.data() + kRecordHeaderBytes, payload_bytes);
    if (crc32c(payload) != payload_crc) {
      stop = ScanStop{true, false, RecoveryIssue::CorruptRecordSalvaged, offset, "record payload checksum does not match"};
      break;
    }
    const Digest256 expected =
        chain_step(chain, std::span<const std::byte>(scratch.data() + 4, kRecordHeaderBytes - 4), payload);
    const auto* stored_chain = reinterpret_cast<const std::uint8_t*>(scratch.data() + kRecordHeaderBytes + payload_bytes);
    if (std::memcmp(stored_chain, expected.bytes.data(), kChainBytes) != 0) {
      stop = ScanStop{true, false, RecoveryIssue::ChainBreak, offset, "record chain digest does not match"};
      break;
    }

    chain = expected;
    offset += static_cast<std::uint64_t>(frame_bytes) + 4;
    ++sequence;
    ++loaded;
  }

  report.records_loaded = loaded;
  store->records_loaded_ = loaded;
  report.bytes_loaded = offset;
  report.bytes_discarded = file_size - offset;
  store->chain_ = chain;
  store->impl_->next_sequence = sequence;

  if (stop.stopped) {
    const bool recoverable = stop.tail_incomplete || options.recovery_policy == RecoveryPolicy::SalvagePrefix;
    report.issue = stop.issue;
    report.detail = stop.detail;
    report.issue_offset = stop.offset;
    if (!recoverable) {
      report.opened = false;
      report.chain_intact = false;
      report.records_rejected = 1;
      const ErrorCode code =
          stop.issue == RecoveryIssue::ChainBreak ? ErrorCode::CorruptStore : ErrorCode::CorruptStore;
      return make_error<std::unique_ptr<Store>>(
          code,
          "store is damaged at offset " + std::to_string(stop.offset) + ": " + stop.detail);
    }
    report.salvaged = true;
    report.chain_intact = false;
    report.records_rejected = 1;
    Outcome<void> truncated = store->impl_->file.Truncate(offset);
    if (!truncated.has_value()) {
      report.opened = false;
      report.issue = RecoveryIssue::IoFailure;
      report.detail = truncated.error().message();
      return make_error<std::unique_ptr<Store>>(truncated.error());
    }
    Outcome<void> synced = store->impl_->file.Sync();
    if (!synced.has_value()) {
      report.opened = false;
      report.issue = RecoveryIssue::IoFailure;
      report.detail = synced.error().message();
      return make_error<std::unique_ptr<Store>>(synced.error());
    }
    report.bytes_discarded = file_size - offset;
  } else {
    report.issue = RecoveryIssue::CleanOpen;
    report.detail = "opened cleanly";
    report.bytes_discarded = 0;
    report.chain_intact = true;
  }

  if (offset > options.max_log_bytes) {
    // The log is over budget. Opening still succeeds: refusing to open a valid
    // store would turn a policy limit into data loss. should_compact() is true
    // immediately, so the caller compacts on the next opportunity.
    report.detail += "; log is over the configured byte budget";
  }

  store->impl_->write_offset = offset;
  store->impl_->valid_end = offset;
  store->bytes_on_disk_ = offset;
  return store;
}

Outcome<void> Store::Append(RecordKind kind, std::span<const std::byte> payload) {
  if (closed_) {
    return make_error(ErrorCode::Closed, "the store is closed");
  }
  if (!replayed_) {
    return make_error(ErrorCode::Internal, "the store must be replayed before it is appended to");
  }
  if (!valid_record_kind(kind)) {
    return make_error(ErrorCode::InvalidArgument, "record kind is not understood");
  }
  if (payload.size() > options_.max_record_bytes) {
    return make_error(ErrorCode::CapacityExceeded,
                      "record of " + std::to_string(payload.size()) + " bytes exceeds the " +
                          std::to_string(options_.max_record_bytes) + " byte limit");
  }

  std::uint64_t frame_bytes = 0;
  if (!checked_add(kRecordHeaderBytes, static_cast<std::uint64_t>(payload.size()), frame_bytes) ||
      !checked_add(frame_bytes, kRecordTrailerBytes, frame_bytes)) {
    return make_error(ErrorCode::CapacityExceeded, "record length overflows");
  }

  const std::uint64_t sequence = impl_->next_sequence;
  std::vector<std::byte>& frame = impl_->scratch;
  frame.assign(static_cast<std::size_t>(frame_bytes), std::byte{0});
  auto frame_view = std::span<std::byte>(frame.data(), frame.size());
  store_u32(frame_view, 0, static_cast<std::uint32_t>(frame_bytes - 4));
  store_u16(frame_view, 4, static_cast<std::uint16_t>(kind));
  store_u16(frame_view, 6, kRecordFlagsNone);
  store_u64(frame_view, 8, sequence);
  store_u32(frame_view, 16, static_cast<std::uint32_t>(payload.size()));
  store_u32(frame_view, 20, crc32c(payload));
  if (!payload.empty()) {
    std::memcpy(frame.data() + kRecordHeaderBytes, payload.data(), payload.size());
  }
  const Digest256 next_chain =
      chain_step(chain_, std::span<const std::byte>(frame.data() + 4, kRecordHeaderBytes - 4), payload);
  std::memcpy(frame.data() + kRecordHeaderBytes + payload.size(), next_chain.bytes.data(), kChainBytes);
  const std::size_t crc_offset = kRecordHeaderBytes + payload.size() + kChainBytes;
  store_u32(frame_view, crc_offset, crc32c(std::span<const std::byte>(frame.data(), crc_offset)));

  Outcome<void> written = impl_->file.WriteAt(impl_->write_offset, frame);
  if (!written.has_value()) {
    return make_error(ErrorCode::PersistenceFailure, written.error().message());
  }
  if (options_.fsync_on_commit) {
    Outcome<void> synced = impl_->file.Sync();
    if (!synced.has_value()) {
      return make_error(ErrorCode::PersistenceFailure, synced.error().message());
    }
  }

  impl_->write_offset += frame_bytes;
  impl_->valid_end = impl_->write_offset;
  impl_->next_sequence = sequence + 1;
  bytes_on_disk_ = impl_->write_offset;
  ++records_written_;
  ++records_since_rewrite_;
  chain_ = next_chain;
  return Outcome<void>();
}

Outcome<void> Store::Sync() {
  if (closed_) {
    return make_error(ErrorCode::Closed, "the store is closed");
  }
  return impl_->file.Sync();
}

Outcome<void> Store::VisitRecords(const RecordVisitor& visitor) {
  if (closed_) {
    return make_error(ErrorCode::Closed, "the store is closed");
  }
  if (replayed_) {
    return make_error(ErrorCode::Internal, "the store has already been replayed");
  }

  std::uint64_t offset = kHeaderBytes;
  const std::uint64_t end = impl_->valid_end;
  std::vector<std::byte> buffer;
  while (offset < end) {
    std::array<std::byte, kRecordHeaderBytes> record_header{};
    std::size_t read = 0;
    Outcome<void> header_result =
        impl_->file.ReadAt(offset, std::span<std::byte>(record_header.data(), record_header.size()), read);
    if (!header_result.has_value()) {
      return make_error(header_result.error());
    }
    if (read != kRecordHeaderBytes) {
      return make_error(ErrorCode::TruncatedStore, "validated record header disappeared during replay");
    }
    const std::uint32_t frame_bytes = load_u32(record_header, 0);
    const auto kind = static_cast<RecordKind>(load_u16(record_header, 4));
    const std::uint32_t payload_bytes = load_u32(record_header, 16);

    buffer.assign(static_cast<std::size_t>(frame_bytes) + 4, std::byte{0});
    std::size_t body_read = 0;
    Outcome<void> body_result = impl_->file.ReadAt(offset, std::span<std::byte>(buffer.data(), buffer.size()), body_read);
    if (!body_result.has_value()) {
      return make_error(body_result.error());
    }
    if (body_read != buffer.size()) {
      return make_error(ErrorCode::TruncatedStore, "validated record disappeared during replay");
    }

    const std::span<const std::byte> payload(buffer.data() + kRecordHeaderBytes, payload_bytes);
    if (!visitor(kind, payload)) {
      break;
    }
    offset += static_cast<std::uint64_t>(frame_bytes) + 4;
  }

  replayed_ = true;
  return Outcome<void>();
}

Outcome<void> Store::Rewrite(std::span<const StoreRecord> records) {
  if (closed_) {
    return make_error(ErrorCode::Closed, "the store is closed");
  }
  if (!replayed_) {
    return make_error(ErrorCode::Internal, "the store must be replayed before it is rewritten");
  }

  const std::filesystem::path temporary = std::filesystem::path(options_.path.string() + ".tmp");
  Outcome<void> removed = detail::remove_file(temporary);
  if (!removed.has_value()) {
    return make_error(removed.error());
  }

  Outcome<detail::FileHandle> handle = detail::FileHandle::Open(temporary, false, true, true);
  if (!handle.has_value()) {
    return make_error(handle.error());
  }
  detail::FileHandle file = std::move(handle).value();

  std::array<std::byte, kHeaderBytes> header{};
  fill_header(header);
  Outcome<void> header_written = file.WriteAt(0, header);
  if (!header_written.has_value()) {
    return make_error(header_written.error());
  }

  std::uint64_t offset = kHeaderBytes;
  Digest256 chain = sha256(std::string_view());
  std::uint64_t sequence = 1;
  std::uint64_t written = 0;
  std::vector<std::byte> frame;
  for (const StoreRecord& record : records) {
    if (!valid_record_kind(record.kind)) {
      return make_error(ErrorCode::InvalidArgument, "rewrite record kind is not understood");
    }
    if (record.payload.size() > options_.max_record_bytes) {
      return make_error(ErrorCode::CapacityExceeded, "rewrite record exceeds the configured record limit");
    }
    std::uint64_t frame_bytes = 0;
    if (!checked_add(kRecordHeaderBytes, static_cast<std::uint64_t>(record.payload.size()), frame_bytes) ||
        !checked_add(frame_bytes, kRecordTrailerBytes, frame_bytes)) {
      return make_error(ErrorCode::CapacityExceeded, "rewrite record length overflows");
    }
    frame.assign(static_cast<std::size_t>(frame_bytes), std::byte{0});
    auto frame_view = std::span<std::byte>(frame.data(), frame.size());
    store_u32(frame_view, 0, static_cast<std::uint32_t>(frame_bytes - 4));
    store_u16(frame_view, 4, static_cast<std::uint16_t>(record.kind));
    store_u16(frame_view, 6, kRecordFlagsNone);
    store_u64(frame_view, 8, sequence);
    store_u32(frame_view, 16, static_cast<std::uint32_t>(record.payload.size()));
    store_u32(frame_view, 20, crc32c(record.payload));
    if (!record.payload.empty()) {
      std::memcpy(frame.data() + kRecordHeaderBytes, record.payload.data(), record.payload.size());
    }
    const Digest256 record_chain =
        chain_step(chain, std::span<const std::byte>(frame.data() + 4, kRecordHeaderBytes - 4), record.payload);
    std::memcpy(frame.data() + kRecordHeaderBytes + record.payload.size(), record_chain.bytes.data(), kChainBytes);
    const std::size_t crc_offset = kRecordHeaderBytes + record.payload.size() + kChainBytes;
    store_u32(frame_view, crc_offset, crc32c(std::span<const std::byte>(frame.data(), crc_offset)));

    Outcome<void> write_result = file.WriteAt(offset, frame);
    if (!write_result.has_value()) {
      return make_error(write_result.error());
    }
    offset += frame_bytes;
    chain = record_chain;
    ++sequence;
    ++written;
  }

  Outcome<void> synced = file.Sync();
  if (!synced.has_value()) {
    return make_error(synced.error());
  }
  file.Close();

  // The handle on the log is released before the rename. Some platforms refuse
  // to replace a file that this process still has open, and the rename is
  // atomic either way: a crash between the close and the rename leaves the
  // original log untouched and a stray temporary that the next open removes.
  impl_->file.Close();
  Outcome<void> replaced = detail::atomic_replace(temporary, options_.path);
  if (!replaced.has_value()) {
    Outcome<void> cleaned = detail::remove_file(temporary);
    (void)cleaned;
    // Put the store back into a usable state against the original log.
    Outcome<detail::FileHandle> restored = detail::FileHandle::Open(options_.path, true, true, false);
    if (restored.has_value()) {
      impl_->file = std::move(restored).value();
    }
    return make_error(replaced.error());
  }

  Outcome<detail::FileHandle> reopened = detail::FileHandle::Open(options_.path, true, true, false);
  if (!reopened.has_value()) {
    return make_error(reopened.error());
  }
  impl_->file = std::move(reopened).value();
  impl_->write_offset = offset;
  impl_->valid_end = offset;
  impl_->next_sequence = sequence;
  bytes_on_disk_ = offset;
  records_written_ = 0;
  records_loaded_ = written;
  records_since_rewrite_ = 0;
  chain_ = chain;
  ++rewrite_count_;
  return Outcome<void>();
}

void Store::Close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  if (impl_) {
    impl_->file.Close();
  }
}

} // namespace cable_registry
