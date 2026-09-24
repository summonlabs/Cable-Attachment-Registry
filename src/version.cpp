// Cable Attachment Registry — version and build identity.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "cable_registry/version.hpp"

namespace cable_registry {

std::string version_string() {
  return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." + std::to_string(kVersionPatch);
}

std::uint32_t version_number() noexcept {
  return (kVersionMajor * 10000u) + (kVersionMinor * 100u) + kVersionPatch;
}

} // namespace cable_registry
