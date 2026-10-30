# SPDX-License-Identifier: LGPL-3.0-or-later
# What a tool says when asked its version (osoe_version_test in
# OsoeVersion.cmake registers it): exactly one line, EXPECTED, on standard
# output, nothing on standard error, and exit status 0. EXPECTED is built from
# the project's version when the test is registered, so the tool is compared
# with what CMake knows, and a version written into one place by hand fails here.
#
#   cmake -DPROGRAM=<path> -DARGUMENTS=--version "-DEXPECTED=osoe-stat (osoe) 0.2.0"
#         -P CheckVersion.cmake
cmake_minimum_required(VERSION 3.25)

foreach(var PROGRAM ARGUMENTS EXPECTED)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

execute_process(COMMAND ${PROGRAM} ${ARGUMENTS}
  OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "${PROGRAM} ${ARGUMENTS} did not exit with status 0: ${rc}")
endif()
if(NOT out STREQUAL "${EXPECTED}\n" OR NOT err STREQUAL "")
  message(FATAL_ERROR
    "${PROGRAM} ${ARGUMENTS} printed '${out}' on standard output and '${err}' on "
    "standard error; it must print the one line '${EXPECTED}' and nothing else")
endif()
message(STATUS "${EXPECTED}")
