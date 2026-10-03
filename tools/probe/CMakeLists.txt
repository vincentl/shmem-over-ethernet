# SPDX-License-Identifier: LGPL-3.0-or-later
# base/ring-check.c is shmem_init's io_uring check, compiled in rather than
# osoe_base linked, which would bring every base object and what they call.
add_executable(probe probe.c ${PROJECT_SOURCE_DIR}/base/ring-check.c)
target_include_directories(probe PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(probe PRIVATE PkgConfig::LIBURING)
osoe_warnings(probe)

# Registered as a test so that `ctest` on a machine whose kernel lacks a
# required facility fails immediately, before any library test runs.
add_test(NAME env.probe COMMAND probe)
set_tests_properties(env.probe PROPERTIES LABELS "env" TIMEOUT 60)
