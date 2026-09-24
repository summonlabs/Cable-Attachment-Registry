// Cable Attachment Registry — observation and receive timestamps.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_TIME_HPP
#define CABLE_REGISTRY_TIME_HPP

#include <cstdint>
#include <string>

#include "cable_registry/export.hpp"

namespace cable_registry {

/// Nanoseconds since the Unix epoch, UTC.
///
/// Two distinct roles are modelled with the same representation and are never
/// interchangeable:
///
///  * the observation time is asserted by the source and is part of the
///    evidence;
///  * the receive time is stamped by the registry when the record is accepted
///    and is provenance only.
///
/// Neither is used to order claims across sources. Ordering inside one source
/// uses Generation, and ordering across sources uses Authority. A record that
/// merely arrived later never wins.
struct Timestamp {
  std::int64_t unix_nanos = 0;

  friend constexpr bool operator==(const Timestamp&, const Timestamp&) noexcept = default;
  friend constexpr auto operator<=>(const Timestamp&, const Timestamp&) noexcept = default;
};

/// The current wall clock reading.
CABLE_REGISTRY_API Timestamp now_timestamp() noexcept;

/// An unset timestamp. Distinct from the epoch, and rejected where a real
/// observation time is required.
inline constexpr Timestamp kUnsetTimestamp{0};

/// RFC 3339 form with nanosecond precision, e.g. 2026-01-31T12:00:00.123456789Z.
CABLE_REGISTRY_API std::string to_rfc3339_utc(Timestamp value);

} // namespace cable_registry

#endif // CABLE_REGISTRY_TIME_HPP
