// Cable Attachment Registry — error classification.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/error.hpp"

namespace cable_registry {

const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "ok";
    case ErrorCode::InvalidArgument:
      return "invalid-argument";
    case ErrorCode::NotFound:
      return "not-found";
    case ErrorCode::CapacityExceeded:
      return "capacity-exceeded";
    case ErrorCode::Conflict:
      return "conflict";
    case ErrorCode::Fenced:
      return "fenced";
    case ErrorCode::UnsupportedVersion:
      return "unsupported-version";
    case ErrorCode::PersistenceFailure:
      return "persistence-failure";
    case ErrorCode::CorruptStore:
      return "corrupt-store";
    case ErrorCode::TruncatedStore:
      return "truncated-store";
    case ErrorCode::IoFailure:
      return "io-failure";
    case ErrorCode::ProtocolViolation:
      return "protocol-violation";
    case ErrorCode::Closed:
      return "closed";
    case ErrorCode::PermissionDenied:
      return "permission-denied";
    case ErrorCode::Internal:
      return "internal";
  }
  return "unknown";
}

std::string Error::describe() const {
  if (ok()) {
    return "ok";
  }
  return std::string(to_string(code_)) + ": " + message_;
}

} // namespace cable_registry
