# Cable Attachment Registry — build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tools, the benchmarks and the tests each carry exactly the
# options they need and nothing this project uses for itself can leak into the
# installed package.

cmake_minimum_required(VERSION 3.20)

project(CableAttachmentRegistry
        VERSION 1.0.0
        DESCRIPTION "Vendor-neutral runtime for physical attachment identity and topology provenance"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(BUILD_SHARED_LIBS "Build cable_registry as a shared library instead of a static one" OFF)

option(CABLE_ATTACHMENT_REGISTRY_BUILD_TESTS "Build the Cable Attachment Registry test suite" ON)
option(CABLE_ATTACHMENT_REGISTRY_BUILD_TOOLS "Build the Cable Attachment Registry command line tools" ON)
option(CABLE_ATTACHMENT_REGISTRY_BUILD_EXAMPLES "Build the Cable Attachment Registry example programs" ON)
option(CABLE_ATTACHMENT_REGISTRY_BUILD_BENCHMARKS "Build the Cable Attachment Registry benchmarks" ON)
option(CABLE_ATTACHMENT_REGISTRY_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(CABLE_ATTACHMENT_REGISTRY_SANITIZERS "Build with AddressSanitizer, and UndefinedBehaviorSanitizer on GCC and Clang" OFF)
option(CABLE_ATTACHMENT_REGISTRY_BUILD_PACKAGE_CONSUMER_TEST
       "Add a test that installs the package and builds the downstream find_package consumer" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CheckCXXCompilerFlag)
include(CTest)

set(CABLE_ATTACHMENT_REGISTRY_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/CableAttachmentRegistry"
    CACHE STRING "Install destination of the Cable Attachment Registry CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Resolves a declared source list against the source tree. A declared file that
# is not on disk yet is marked GENERATED so an incremental build still
# configures; the build then fails loudly on the missing translation unit
# rather than silently dropping it.
function(cable_attachment_registry_sources out_var)
  set(resolved "")
  set(missing "")
  foreach(source IN LISTS ARGN)
    if(IS_ABSOLUTE "${source}")
      set(path "${source}")
    else()
      set(path "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    endif()
    if(EXISTS "${path}")
      list(APPEND resolved "${source}")
    else()
      set_source_files_properties("${path}" PROPERTIES GENERATED TRUE)
      list(APPEND resolved "${path}")
      list(APPEND missing "${source}")
    endif()
  endforeach()
  if(missing)
    list(JOIN missing ", " missing_text)
    message(WARNING "CableAttachmentRegistry: declared but not on disk: ${missing_text}")
  endif()
  set(${out_var} "${resolved}" PARENT_SCOPE)
endfunction()

# Applies the project build policy to one target. Every entry is PRIVATE, so
# none of it is part of the installed package.
function(cable_attachment_registry_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc /MP)
    if(CABLE_ATTACHMENT_REGISTRY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/RTC1>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(CABLE_ATTACHMENT_REGISTRY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(CABLE_ATTACHMENT_REGISTRY_SANITIZERS)
    target_compile_options(${target} PRIVATE ${CABLE_ATTACHMENT_REGISTRY_SANITIZER_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${CABLE_ATTACHMENT_REGISTRY_SANITIZER_LINK_OPTIONS})
  endif()
endfunction()

# Adds one project executable: source resolution, linkage against the library
# and the project build policy.
function(cable_attachment_registry_add_executable target)
  cable_attachment_registry_sources(sources ${ARGN})
  add_executable(${target} ${sources})
  target_link_libraries(${target} PRIVATE cable_registry)
  cable_attachment_registry_configure_target(${target})
endfunction()

# ---------------------------------------------------------------------------
# Sanitizers
# ---------------------------------------------------------------------------

if(CABLE_ATTACHMENT_REGISTRY_SANITIZERS)
  if(MSVC)
    check_cxx_compiler_flag("/fsanitize=address" CABLE_ATTACHMENT_REGISTRY_COMPILER_HAS_ASAN)
    if(NOT CABLE_ATTACHMENT_REGISTRY_COMPILER_HAS_ASAN)
      message(FATAL_ERROR
        "CABLE_ATTACHMENT_REGISTRY_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: a program built with /fsanitize=address does not compile "
        "and link here. Either the toolset predates AddressSanitizer support or the optional "
        "C++ AddressSanitizer component is not installed. Reconfigure with "
        "-DCABLE_ATTACHMENT_REGISTRY_SANITIZERS=OFF or install that component.")
    endif()
    set(CABLE_ATTACHMENT_REGISTRY_SANITIZER_COMPILE_OPTIONS /fsanitize=address /Zi)
    set(CABLE_ATTACHMENT_REGISTRY_SANITIZER_LINK_OPTIONS /fsanitize=address)
    message(STATUS "CableAttachmentRegistry: AddressSanitizer enabled (MSVC)")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    check_cxx_compiler_flag("-fsanitize=address,undefined" CABLE_ATTACHMENT_REGISTRY_COMPILER_HAS_ASAN_UBSAN)
    if(NOT CABLE_ATTACHMENT_REGISTRY_COMPILER_HAS_ASAN_UBSAN)
      message(FATAL_ERROR
        "CABLE_ATTACHMENT_REGISTRY_SANITIZERS=ON cannot be used with ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}: the sanitizer runtime is missing. Reconfigure with "
        "-DCABLE_ATTACHMENT_REGISTRY_SANITIZERS=OFF or install it.")
    endif()
    set(CABLE_ATTACHMENT_REGISTRY_SANITIZER_COMPILE_OPTIONS -fsanitize=address,undefined -fno-omit-frame-pointer)
    set(CABLE_ATTACHMENT_REGISTRY_SANITIZER_LINK_OPTIONS -fsanitize=address,undefined)
    message(STATUS "CableAttachmentRegistry: AddressSanitizer and UndefinedBehaviorSanitizer enabled")
  else()
    message(FATAL_ERROR
      "CABLE_ATTACHMENT_REGISTRY_SANITIZERS=ON is supported with MSVC (/fsanitize=address) and with "
      "GCC or Clang (-fsanitize=address,undefined); the configured compiler is "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}.")
  endif()
endif()

# ---------------------------------------------------------------------------
# cable_registry
# ---------------------------------------------------------------------------

set(CABLE_ATTACHMENT_REGISTRY_SOURCES
  src/version.cpp
  src/ids.cpp
  src/time.cpp
  src/error.cpp
  src/capability.cpp
  src/authority.cpp
  src/identity.cpp
  src/evidence.cpp
  src/limits.cpp
  src/digest.cpp
  src/codec.cpp
  src/serialization.cpp
  src/snapshot.cpp
  src/registry_state.cpp
  src/registry.cpp
  src/persistence.cpp
  src/graph.cpp
  src/render.cpp
  src/detail/file_io.cpp
  src/ipc/protocol.cpp
  src/ipc/socket.cpp
  src/ipc/server.cpp
  src/ipc/client.cpp)

cable_attachment_registry_sources(CABLE_ATTACHMENT_REGISTRY_RESOLVED_SOURCES ${CABLE_ATTACHMENT_REGISTRY_SOURCES})

add_library(cable_registry ${CABLE_ATTACHMENT_REGISTRY_RESOLVED_SOURCES})
add_library(SummonSoftwareLabs::CableAttachmentRegistry ALIAS cable_registry)

set_target_properties(cable_registry PROPERTIES
  EXPORT_NAME CableAttachmentRegistry
  OUTPUT_NAME cable_registry
  VERSION ${PROJECT_VERSION}
  SOVERSION 1
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)

if(BUILD_SHARED_LIBS)
  set_target_properties(cable_registry PROPERTIES CXX_VISIBILITY_PRESET hidden)
  target_compile_definitions(cable_registry PRIVATE CABLE_REGISTRY_BUILD_SHARED)
  target_compile_definitions(cable_registry INTERFACE CABLE_REGISTRY_SHARED)
endif()

target_include_directories(cable_registry
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(cable_registry PUBLIC cxx_std_20)

if(WIN32)
  # Winsock for the publisher transport. PRIVATE: a static consumer still
  # receives it through LINK_ONLY in the exported target.
  target_link_libraries(cable_registry PRIVATE ws2_32)
endif()

cable_attachment_registry_configure_target(cable_registry)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

if(CABLE_ATTACHMENT_REGISTRY_BUILD_TOOLS)
  cable_attachment_registry_add_executable(cable-registry-daemon tools/daemon_main.cpp)
  cable_attachment_registry_add_executable(cable-registry-cli tools/cli_main.cpp)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(CABLE_ATTACHMENT_REGISTRY_BUILD_EXAMPLES)
  file(GLOB CABLE_ATTACHMENT_REGISTRY_EXAMPLE_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.cpp")
  foreach(example_source IN LISTS CABLE_ATTACHMENT_REGISTRY_EXAMPLE_SOURCES)
    get_filename_component(example_stem "${example_source}" NAME_WE)
    cable_attachment_registry_add_executable(cable-registry-example-${example_stem} "${example_source}")
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(CABLE_ATTACHMENT_REGISTRY_BUILD_BENCHMARKS)
  cable_attachment_registry_add_executable(cable-registry-benchmarks benchmarks/benchmark_registry.cpp)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(BUILD_TESTING AND CABLE_ATTACHMENT_REGISTRY_BUILD_TESTS)
  # One executable per translation unit in tests/. tests/support is the shared
  # harness; tests/package_consumer is a standalone downstream project that is
  # configured separately against the installed package.
  file(GLOB CABLE_ATTACHMENT_REGISTRY_TEST_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  file(GLOB CABLE_ATTACHMENT_REGISTRY_TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp")

  if(CABLE_ATTACHMENT_REGISTRY_TEST_SUPPORT_SOURCES)
    add_library(cable_registry_test_support STATIC ${CABLE_ATTACHMENT_REGISTRY_TEST_SUPPORT_SOURCES})
    set_target_properties(cable_registry_test_support PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(cable_registry_test_support PUBLIC cable_registry)
    target_include_directories(cable_registry_test_support PUBLIC
      "${CMAKE_CURRENT_SOURCE_DIR}/tests"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/support")
    cable_attachment_registry_configure_target(cable_registry_test_support)
  endif()

  foreach(test_source IN LISTS CABLE_ATTACHMENT_REGISTRY_TEST_SOURCES)
    get_filename_component(test_stem "${test_source}" NAME_WE)
    string(REGEX REPLACE "^test_" "" test_suffix "${test_stem}")
    set(test_target "cable-registry-test-${test_suffix}")

    cable_attachment_registry_add_executable(${test_target} "${test_source}")
    if(TARGET cable_registry_test_support)
      target_link_libraries(${test_target} PRIVATE cable_registry_test_support)
    endif()
    if(TARGET cable-registry-daemon AND TARGET cable-registry-cli)
      add_dependencies(${test_target} cable-registry-daemon cable-registry-cli)
    endif()

    if(test_stem STREQUAL "test_multiprocess" AND TARGET cable-registry-daemon AND TARGET cable-registry-cli)
      add_test(NAME ${test_target}
               COMMAND ${test_target}
                       --daemon $<TARGET_FILE:cable-registry-daemon>
                       --cli $<TARGET_FILE:cable-registry-cli>)
    else()
      add_test(NAME ${test_target} COMMAND ${test_target})
    endif()
  endforeach()

  if(CABLE_ATTACHMENT_REGISTRY_BUILD_PACKAGE_CONSUMER_TEST AND NOT CABLE_ATTACHMENT_REGISTRY_SANITIZERS)
    add_test(NAME cable-registry-test-package-consumer
             COMMAND ${CMAKE_COMMAND}
                     -D "CONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/package_consumer"
                     -D "BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                     -D "WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-consumer-check"
                     -D "CONFIG=$<CONFIG>"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PackageConsumerCheck.cmake")
  endif()
endif()

# ---------------------------------------------------------------------------
# Installation and packaging
# ---------------------------------------------------------------------------

install(TARGETS cable_registry
        EXPORT CableAttachmentRegistryTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT CableAttachmentRegistryTargets
        NAMESPACE SummonSoftwareLabs::
        DESTINATION ${CABLE_ATTACHMENT_REGISTRY_INSTALL_CMAKEDIR})

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CableAttachmentRegistryConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/CableAttachmentRegistryConfig.cmake"
  INSTALL_DESTINATION "${CABLE_ATTACHMENT_REGISTRY_INSTALL_CMAKEDIR}"
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/CableAttachmentRegistryConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/CableAttachmentRegistryConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/CableAttachmentRegistryConfigVersion.cmake"
        DESTINATION ${CABLE_ATTACHMENT_REGISTRY_INSTALL_CMAKEDIR})

# The build tree is usable by a sibling project without installing first.
export(EXPORT CableAttachmentRegistryTargets
       NAMESPACE SummonSoftwareLabs::
       FILE "${CMAKE_CURRENT_BINARY_DIR}/CableAttachmentRegistryTargets.cmake")
