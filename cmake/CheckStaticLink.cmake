# SPDX-License-Identifier: LGPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)

# Configures and builds libosoe as a static archive in a scratch directory,
# then compiles and links a tiny consumer against that archive alone - never
# against osoe_base, which base/CMakeLists.txt never installs. This is the
# portability review's F10 reproduction: `-DBUILD_SHARED_LIBS=OFF` built and
# installed a libosoe.a that a third party could not link, because osoe_base's
# objects lived in a second archive nobody ships. Nothing else exercises the
# static variant - none of the four presets sets BUILD_SHARED_LIBS - so this
# runs its own nested configure rather than relying on the caller's build.
#
# Since phase 1 step 3 libosoe uses liburing, which a consumer of the static
# archive links itself, exactly as it would for any static library with a
# dependency; the distribution's liburing is the one §1 allows (invariant 15).
# The single-node build has none, and URING_LIBRARIES is then empty: SINGLE_NODE
# is the caller's OSOE_SINGLE_NODE, so that the nested build is the same kind.
#
#   cmake -DSOURCE_DIR=<repo root> -DSCRATCH_DIR=<scratch build dir>
#         -DPROBE_SRC=<probe.c> -DC_COMPILER=<cc> -DURING_LIBRARIES=<liburing>
#         [-DSINGLE_NODE=ON|OFF] -P CheckStaticLink.cmake

foreach(var SOURCE_DIR SCRATCH_DIR PROBE_SRC C_COMPILER URING_LIBRARIES)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()
if(NOT DEFINED SINGLE_NODE)
  set(SINGLE_NODE OFF)
endif()

file(REMOVE_RECURSE "${SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SCRATCH_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${SCRATCH_DIR}"
          -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
          -DOSOE_SINGLE_NODE=${SINGLE_NODE}
  OUTPUT_VARIABLE configure_out ERROR_VARIABLE configure_err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Configuring a static build failed:\n${configure_out}\n${configure_err}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${SCRATCH_DIR}" --target osoe
  OUTPUT_VARIABLE build_out ERROR_VARIABLE build_err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Building libosoe statically failed:\n${build_out}\n${build_err}")
endif()
# make's own notices of clock skew are not the compiler's warnings: a build
# tree on a filesystem shared with its host can see its files a moment in the
# future, and make says so.
string(REGEX REPLACE "[^\n]*(Clock skew detected|modification time [^\n]* in the future)[^\n]*"
       "" build_out "${build_out}")
string(REGEX REPLACE "[^\n]*(Clock skew detected|modification time [^\n]* in the future)[^\n]*"
       "" build_err "${build_err}")
if(build_out MATCHES "warning:" OR build_err MATCHES "warning:")
  message(FATAL_ERROR "Building libosoe statically warned:\n${build_out}\n${build_err}")
endif()

file(GLOB_RECURSE archive "${SCRATCH_DIR}/*/libosoe.a")
list(LENGTH archive count)
if(NOT count EQUAL 1)
  message(FATAL_ERROR "Expected exactly one libosoe.a under ${SCRATCH_DIR}, found: ${archive}")
endif()

set(probe_object "${SCRATCH_DIR}/static-link-probe.o")
set(probe_exe "${SCRATCH_DIR}/static-link-probe")

execute_process(
  COMMAND "${C_COMPILER}" -std=c11 -I "${SOURCE_DIR}/shmem/include"
          -c "${PROBE_SRC}" -o "${probe_object}"
  OUTPUT_VARIABLE cc_out ERROR_VARIABLE cc_err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Compiling the static-link probe failed:\n${cc_out}\n${cc_err}")
endif()

execute_process(
  COMMAND "${C_COMPILER}" "${probe_object}" "${archive}" ${URING_LIBRARIES} -pthread
          -o "${probe_exe}"
  OUTPUT_VARIABLE link_out ERROR_VARIABLE link_err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "Linking a program against libosoe.a alone failed (F10):\n${link_out}\n${link_err}")
endif()

execute_process(COMMAND "${probe_exe}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "The program linked against libosoe.a alone exited ${rc}")
endif()

if(SINGLE_NODE)
  message(STATUS "libosoe.a alone (no separate osoe_base archive), the single-node build's, "
                 "links and runs a consumer with no liburing")
else()
  message(STATUS "libosoe.a alone (no separate osoe_base archive), with the distribution's "
                 "liburing, links and runs a consumer")
endif()
