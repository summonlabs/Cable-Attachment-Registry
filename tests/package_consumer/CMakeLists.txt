# Standalone downstream project.
#
# This is deliberately NOT part of the Cable Attachment Registry build: it
# stands in for a real consumer and is configured on its own against an
# installed package to prove that the exported CMake package works.
#
#   cmake -S tests/package_consumer -B <dir> -DCableAttachmentRegistry_DIR=<prefix>/lib/cmake/CableAttachmentRegistry

cmake_minimum_required(VERSION 3.20)

project(CableAttachmentRegistryPackageConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CableAttachmentRegistry 1.0 CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE SummonSoftwareLabs::CableAttachmentRegistry)
