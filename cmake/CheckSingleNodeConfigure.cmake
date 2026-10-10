# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Configures the single-node build (OSOE_SINGLE_NODE=ON) of this tree in a
# scratch directory, from the full build's suite, and fails if CMake refuses
# it. The two builds share their lists, and a line that holds only in the full
# build - a property set on a test only it registers, a target only it builds -
# refuses the single-node build at configure time, where nothing in the full
# build's suite looks: shmem.helper-wake's RUN_SERIAL did so from the grant
# fields' merge (fcb9cf0) until this check. Configure and generate only, since
# compiling and running the single-node build is its own suite's.
#
# The nested configure is the caller's own kind - its build type, its C and
# linker flags, its shared or static library and its timing pins - so that each
# preset's single-node build is the one configured; not OSOE_TIER3, which the
# single-node build refuses by design. C_COMPILER, C_COMPILER_TARGET and
# TOOLCHAIN_FILE are the caller's, as in CheckStrictC11.cmake.
#
#   cmake -DSOURCE_DIR=<repo root> -DSCRATCH_DIR=<scratch build dir>
#         [-DBUILD_TYPE=<type>] [-DC_FLAGS=<flags>] [-DEXE_LINKER_FLAGS=<flags>]
#         [-DSHARED_LINKER_FLAGS=<flags>] [-DSHARED_LIBS=ON|OFF] [-DTEST_PINS=ON|OFF]
#         [-DC_COMPILER=<cc>] [-DC_COMPILER_TARGET=<target>]
#         [-DTOOLCHAIN_FILE=<file>] -P CheckSingleNodeConfigure.cmake

foreach(var SOURCE_DIR SCRATCH_DIR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

set(options)
if(BUILD_TYPE)
  list(APPEND options "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
endif()
if(C_FLAGS)
  list(APPEND options "-DCMAKE_C_FLAGS=${C_FLAGS}")
endif()
if(EXE_LINKER_FLAGS)
  list(APPEND options "-DCMAKE_EXE_LINKER_FLAGS=${EXE_LINKER_FLAGS}")
endif()
if(SHARED_LINKER_FLAGS)
  list(APPEND options "-DCMAKE_SHARED_LINKER_FLAGS=${SHARED_LINKER_FLAGS}")
endif()
if(DEFINED SHARED_LIBS AND NOT SHARED_LIBS STREQUAL "")
  list(APPEND options "-DBUILD_SHARED_LIBS=${SHARED_LIBS}")
endif()
if(DEFINED TEST_PINS AND NOT TEST_PINS STREQUAL "")
  list(APPEND options "-DOSOE_TEST_PINS=${TEST_PINS}")
endif()
if(C_COMPILER)
  list(APPEND options "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()
if(C_COMPILER_TARGET)
  list(APPEND options "-DCMAKE_C_COMPILER_TARGET=${C_COMPILER_TARGET}")
endif()
if(TOOLCHAIN_FILE)
  list(APPEND options "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
endif()

file(REMOVE_RECURSE "${SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SCRATCH_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${SCRATCH_DIR}"
          -DBUILD_TESTING=ON -DOSOE_SINGLE_NODE=ON ${options}
  OUTPUT_VARIABLE configure_out ERROR_VARIABLE configure_err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Configuring the single-node build (OSOE_SINGLE_NODE=ON) failed:\n"
                      "${configure_out}\n${configure_err}")
endif()

message(STATUS "The single-node build configures (OSOE_SINGLE_NODE=ON)")
