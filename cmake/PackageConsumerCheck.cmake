# Installs the freshly built package into a scratch prefix, configures the
# standalone downstream project against it, builds it, and runs it. The consumer
# never sees the source tree or the build tree, so a package that is missing a
# header, a target property or a link dependency fails here.

if(NOT DEFINED CONSUMER_SOURCE_DIR OR NOT DEFINED BUILD_DIR OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "PackageConsumerCheck.cmake needs CONSUMER_SOURCE_DIR, BUILD_DIR and WORK_DIR")
endif()

if(NOT DEFINED CONFIG OR CONFIG STREQUAL "")
  set(CONFIG Release)
endif()

set(prefix "${WORK_DIR}/prefix")
set(consumer_build "${WORK_DIR}/build")
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

message(STATUS "installing the package into ${WORK_DIR}/prefix")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}" --config "${CONFIG}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "cmake --install failed (${install_result})\n${install_output}\n${install_error}")
endif()

set(config_file "${prefix}/lib/cmake/CableAttachmentRegistry/CableAttachmentRegistryConfig.cmake")
if(NOT EXISTS "${config_file}")
  message(FATAL_ERROR "the installed package config was not found at ${config_file}")
endif()

message(STATUS "configuring the downstream consumer against ${prefix}")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${CONSUMER_SOURCE_DIR}"
          -B "${consumer_build}"
          -D "CMAKE_PREFIX_PATH=${prefix}"
          -D "CMAKE_BUILD_TYPE=${CONFIG}"
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "the consumer did not configure (${configure_result})\n${configure_output}\n${configure_error}")
endif()

message(STATUS "building the downstream consumer")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config "${CONFIG}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "the consumer did not build (${build_result})\n${build_output}\n${build_error}")
endif()

set(consumer_exe "${consumer_build}/consumer")
if(NOT EXISTS "${consumer_exe}")
  set(consumer_exe "${consumer_build}/${CONFIG}/consumer.exe")
endif()
if(NOT EXISTS "${consumer_exe}")
  set(consumer_exe "${consumer_build}/consumer.exe")
endif()
if(NOT EXISTS "${consumer_exe}")
  message(FATAL_ERROR "the consumer executable was not produced in ${consumer_build}")
endif()

message(STATUS "running the downstream consumer")
execute_process(
  COMMAND "${consumer_exe}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "the consumer failed (${run_result})\n${run_output}\n${run_error}")
endif()

message(STATUS "downstream package check ok: ${run_output}")
