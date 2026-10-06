# SPDX-License-Identifier: LGPL-3.0-or-later
# The probe checks what the library of its own build uses, and the build chooses
# the files, as it does the library's (OSOE_SINGLE_NODE, the top-level
# CMakeLists.txt). probe.c has the checks both builds make, and main; with it
# the single-node build compiles probe-single-node.c, the classic futex and no
# io_uring, so no liburing, and the full build probe-full-build.c, the futex2
# calls and every io_uring check.
if(OSOE_SINGLE_NODE)
  add_executable(probe probe.c probe-single-node.c)
else()
  # base/ring-check.c is shmem_init's io_uring check, compiled in rather than
  # osoe_base linked, which would bring every base object and what they call.
  add_executable(probe probe.c probe-full-build.c ${PROJECT_SOURCE_DIR}/base/ring-check.c)
  target_link_libraries(probe PRIVATE PkgConfig::LIBURING)
endif()
target_include_directories(probe PRIVATE ${PROJECT_SOURCE_DIR})
osoe_warnings(probe)

# Registered as a test so that `ctest` on a machine whose kernel lacks a
# required facility fails immediately, before any library test runs. Both
# builds register it, each probing what its own library needs.
add_test(NAME env.probe COMMAND probe)
set_tests_properties(env.probe PROPERTIES LABELS "env" TIMEOUT 60)
