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
#   cmake -DSOURCE_DIR=<repo root> -DSCRATCH_DIR=<scratch build dir>
#         -P CheckStrictC11.cmake

foreach(var SOURCE_DIR SCRATCH_DIR)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "${var} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SCRATCH_DIR}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${SCRATCH_DIR}"
          -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_EXTENSIONS=OFF -DBUILD_TESTING=ON
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
if(build_out MATCHES "warning:" OR build_err MATCHES "warning:")
  message(FATAL_ERROR "Building with -std=c11 warned (F12):\n${build_out}\n${build_err}")
endif()

message(STATUS "The whole tree builds warning-free under -std=c11 (CMAKE_C_EXTENSIONS=OFF)")
