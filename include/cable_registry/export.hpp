// Cable Attachment Registry — shared library export decoration.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CABLE_REGISTRY_EXPORT_HPP
#define CABLE_REGISTRY_EXPORT_HPP

// A static build defines nothing and the macro expands to nothing. A shared
// build defines CABLE_REGISTRY_SHARED for consumers and CABLE_REGISTRY_BUILD_SHARED
// while compiling the library itself, so the same headers declare the public
// surface as importable or exportable without a second set of headers.
#if defined(CABLE_REGISTRY_SHARED)
#if defined(_WIN32) || defined(_WIN64)
#if defined(CABLE_REGISTRY_BUILD_SHARED)
#define CABLE_REGISTRY_API __declspec(dllexport)
#else
#define CABLE_REGISTRY_API __declspec(dllimport)
#endif
#else
#define CABLE_REGISTRY_API __attribute__((visibility("default")))
#endif
#else
#define CABLE_REGISTRY_API
#endif

#endif // CABLE_REGISTRY_EXPORT_HPP
