// Cable Attachment Registry — typed errors and outcome values.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_ERROR_HPP
#define CABLE_REGISTRY_ERROR_HPP

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "cable_registry/export.hpp"

namespace cable_registry {

/// Coarse failure classification. A failure is never reported as success and
/// no two distinct conditions share a code.
enum class ErrorCode : std::uint16_t {
  Ok = 0,
  InvalidArgument,
  NotFound,
  CapacityExceeded,
  Conflict,
  Fenced,
  UnsupportedVersion,
  PersistenceFailure,
  CorruptStore,
  TruncatedStore,
  IoFailure,
  ProtocolViolation,
  Closed,
  PermissionDenied,
  Internal,
};

CABLE_REGISTRY_API const char* to_string(ErrorCode code) noexcept;

/// A failure with a human readable explanation.
class CABLE_REGISTRY_API Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] explicit operator bool() const noexcept { return !ok(); }

  /// "code: message", the form the command line tools print.
  [[nodiscard]] std::string describe() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
};

/// Either a value or an Error. There is no implicit conversion from Error to a
/// value: a caller that wants the value must handle the failure.
template <class T>
class [[nodiscard]] Outcome {
 public:
  Outcome(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Outcome(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] const Error& error() const { return std::get<1>(storage_); }

  [[nodiscard]] T value_or(T fallback) const {
    return has_value() ? std::get<0>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Outcome<void> {
 public:
  Outcome() = default;
  Outcome(Error error) : error_(std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept { return error_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const Error& error() const { return error_; }

 private:
  Error error_;
};

/// Builds a failed outcome from an existing error.
template <class T = void>
[[nodiscard]] Outcome<T> make_error(Error error) {
  if constexpr (std::is_void_v<T>) {
    return Outcome<void>(std::move(error));
  } else {
    return Outcome<T>(std::move(error));
  }
}

/// Builds a failed outcome.
template <class T = void>
[[nodiscard]] Outcome<T> make_error(ErrorCode code, std::string message) {
  if constexpr (std::is_void_v<T>) {
    return Outcome<void>(Error(code, std::move(message)));
  } else {
    return Outcome<T>(Error(code, std::move(message)));
  }
}

} // namespace cable_registry

#endif // CABLE_REGISTRY_ERROR_HPP
