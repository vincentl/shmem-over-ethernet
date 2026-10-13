# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Configures and builds the whole tree in a scratch directory with
# `CMAKE_C_EXTENSIONS OFF`, i.e. `-std=c11` rather than the four presets'
# `-std=gnu11`, and fails if anything does not compile clean. This is the
# portability review's F12: the design says "a C11 compiler" but several
# files used clock_gettime, nanosleep, pthread_sigmask, kill, ftruncate and
# setenv with no feature-test macro, relying on GNU's default exposure level
# to declare them. None of the four presets sets CMAKE_C_EXTENSIONS, so
# nothing else ever builds this configuration; this runs its own nested
# configure rather than changing what the presets mean.
#
# Only the configure+build is exercised, deliberately: F12 is a compile-time
# problem (undeclared functions), and BUILD_TESTING=ON so every test's own
# translation unit is compiled and linked too - the review found missing
# macros in eight test files, not only in shmem/src and base/. Running the
# resulting binaries would add a great deal of time for no more signal.
#
# SINGLE_NODE is the caller's OSOE_SINGLE_NODE, so that the nested build is
# the same kind as the one that runs the check: the single-node build's on a
# machine with no liburing, where the full build would not configure.
#
# C_COMPILER, C_COMPILER_TARGET and TOOLCHAIN_FILE are the caller's, so that
# the nested build compiles with the compiler the build that runs the check
# was configured with, not the default cc (the v0.2 step 1 review,
# portability Q2). Its C flags are not passed: the presets' carry the
# sanitizers, which this check does not want.
#
# PKG_CONFIG_LIBDIR, PKG_CONFIG_PATH and PKG_CONFIG_SYSROOT_DIR are the
# environment's when the caller was configured, each set here for the nested
# configure, or unset where it was empty: a cross build names its target's
# liburing so, and ctest's environment need not - with its own the nested
# configure found the host's liburing, and a cross build's nested link failed
# on it (the ninth review's P9-5).
#
#   cmake -DSOURCE_DIR=<repo root> -DSCRATCH_DIR=<scratch build dir>
#         [-DC_COMPILER=<cc>] [-DC_COMPILER_TARGET=<target>]
#         [-DTOOLCHAIN_FILE=<file>] [-DSINGLE_NODE=ON|OFF]
#         [-DPKG_CONFIG_LIBDIR=<dirs>] [-DPKG_CONFIG_PATH=<dirs>]
#         [-DPKG_CONFIG_SYSROOT_DIR=<dir>] -P CheckStrictC11.cmake

foreach(var SOURCE_DIR SCRATCH_DIR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()
if(NOT DEFINED SINGLE_NODE)
  set(SINGLE_NODE OFF)
endif()

set(compiler_options)
if(C_COMPILER)
  list(APPEND compiler_options "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()
if(C_COMPILER_TARGET)
  list(APPEND compiler_options "-DCMAKE_C_COMPILER_TARGET=${C_COMPILER_TARGET}")
endif()
if(TOOLCHAIN_FILE)
  list(APPEND compiler_options "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
endif()

foreach(var PKG_CONFIG_LIBDIR PKG_CONFIG_PATH PKG_CONFIG_SYSROOT_DIR)
  if(NOT DEFINED ${var})
    continue()
  elseif("${${var}}" STREQUAL "")
    unset(ENV{${var}})
  else()
    set(ENV{${var}} "${${var}}")
  endif()
endforeach()

file(REMOVE_RECURSE "${SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SCRATCH_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${SCRATCH_DIR}"
          -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_EXTENSIONS=OFF -DBUILD_TESTING=ON
          -DOSOE_SINGLE_NODE=${SINGLE_NODE} ${compiler_options}
  OUTPUT_VARIABLE configure_out ERROR_VARIABLE configure_err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Configuring with CMAKE_C_EXTENSIONS=OFF failed:\n${configure_out}\n${configure_err}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${SCRATCH_DIR}"
  OUTPUT_VARIABLE build_out ERROR_VARIABLE build_err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Building with -std=c11 failed (F12):\n${build_out}\n${build_err}")
endif()
# make's own notices of clock skew are not the compiler's warnings: a build
# tree on a filesystem shared with its host can see its files a moment in the
# future, and make says so.
string(REGEX REPLACE "[^\n]*(Clock skew detected|modification time [^\n]* in the future)[^\n]*"
       "" build_out "${build_out}")
string(REGEX REPLACE "[^\n]*(Clock skew detected|modification time [^\n]* in the future)[^\n]*"
       "" build_err "${build_err}")
if(build_out MATCHES "warning:" OR build_err MATCHES "warning:")
  message(FATAL_ERROR "Building with -std=c11 warned (F12):\n${build_out}\n${build_err}")
endif()

message(STATUS "The whole tree builds warning-free under -std=c11 (CMAKE_C_EXTENSIONS=OFF)")
