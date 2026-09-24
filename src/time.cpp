// Cable Attachment Registry — timestamps.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/time.hpp"

#include <chrono>
#include <cstdio>

namespace cable_registry {
namespace {

/// Civil date from a count of days since 1970-01-01 (Howard Hinnant's
/// algorithm), so the conversion does not depend on the C library's time zone
/// handling.
void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto doe = static_cast<std::uint64_t>(days - (era * 146097));
  const std::uint64_t yoe = (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + (era * 400);
  const std::uint64_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
  const std::uint64_t mp = ((5 * doy) + 2) / 153;
  day = static_cast<unsigned>(doy - (((153 * mp) + 2) / 5) + 1);
  month = static_cast<unsigned>(mp + (mp < 10 ? 3 : -9));
  year = y + (month <= 2 ? 1 : 0);
}

} // namespace

Timestamp now_timestamp() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return Timestamp{std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()};
}

std::string to_rfc3339_utc(Timestamp value) {
  std::int64_t nanos = value.unix_nanos;
  std::int64_t seconds = nanos / 1'000'000'000;
  std::int64_t fraction = nanos % 1'000'000'000;
  if (fraction < 0) {
    fraction += 1'000'000'000;
    --seconds;
  }
  std::int64_t days = seconds / 86'400;
  std::int64_t second_of_day = seconds % 86'400;
  if (second_of_day < 0) {
    // Floor division: a negative remainder borrows a whole day, otherwise an
    // instant before the epoch renders with the wrong date.
    second_of_day += 86'400;
    --days;
  }

  std::int64_t year = 0;
  unsigned month = 1;
  unsigned day = 1;
  civil_from_days(days, year, month, day);

  const auto hour = static_cast<unsigned>(second_of_day / 3600);
  const auto minute = static_cast<unsigned>((second_of_day % 3600) / 60);
  const auto second = static_cast<unsigned>(second_of_day % 60);

  char buffer[40] = {};
  std::snprintf(buffer,
                sizeof(buffer),
                "%04lld-%02u-%02uT%02u:%02u:%02u.%09lldZ",
                static_cast<long long>(year),
                month,
                day,
                hour,
                minute,
                second,
                static_cast<long long>(fraction));
  return std::string(buffer);
}

} // namespace cable_registry
