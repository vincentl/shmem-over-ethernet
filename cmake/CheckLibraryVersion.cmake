# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Fails unless the shared library's file is named for the project's version and
# its soname for that version's major number - the library's VERSION and
# SOVERSION properties, which come from the one value project(osoe VERSION ...)
# holds (cmake/OsoeVersion.cmake).
#
#   cmake -DLIB=<path to libosoe.so.<version>> -DVERSION=<the project's>
#         -DSOVERSION=<its major> -P CheckLibraryVersion.cmake
#
# LIB is $<TARGET_FILE:osoe>: the real file, libosoe.so.<version>, with
# libosoe.so.<soversion> and libosoe.so as links beside it.
foreach(var LIB VERSION SOVERSION)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

get_filename_component(file "${LIB}" NAME)
if(NOT file STREQUAL "libosoe.so.${VERSION}")
  message(FATAL_ERROR "The shared library is ${file}, not libosoe.so.${VERSION}")
endif()

find_program(READELF readelf REQUIRED)
execute_process(COMMAND "${READELF}" -d "${LIB}"
  OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "readelf failed on ${LIB}")
endif()
if(NOT out MATCHES "[(]SONAME[)] +Library soname: \\[(libosoe[.]so[.][0-9.]*)\\]")
  message(FATAL_ERROR "${LIB} names no soname:\n${out}")
endif()
if(NOT CMAKE_MATCH_1 STREQUAL "libosoe.so.${SOVERSION}")
  message(FATAL_ERROR "The soname is ${CMAKE_MATCH_1}, not libosoe.so.${SOVERSION}")
endif()
message(STATUS "${file}, soname ${CMAKE_MATCH_1}")
