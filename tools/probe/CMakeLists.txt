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
# And its reopen check with the child 200 ms slow to its first load: the
# parent's second value waits for the child's word, where on a timer of 50 ms a
# slow child read it first and the check failed (T34).
add_test(NAME env.probe.slow-reopen COMMAND probe --slow-reopen)
set_tests_properties(env.probe.slow-reopen PROPERTIES LABELS "env" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "ok +unlinked shm object reopened via /proc")

# The full build's probe says whether the kernel has the zero-copy send's
# opcode, which OSOE_WIRE_ZERO_COPY=on needs and nothing else does.
if(NOT OSOE_SINGLE_NODE)
  add_test(NAME env.probe.zero-copy-line
    COMMAND sh -c "$<TARGET_FILE:probe> | grep -q '^info opcode SEND_ZC, for OSOE_WIRE_ZERO_COPY=on: \\(supported\\|missing\\)$'")
  set_tests_properties(env.probe.zero-copy-line PROPERTIES LABELS "env" TIMEOUT 60)
  # The full build's probe links liburing, so the test carries `internode` as
  # every test of the full build's alone does (cmake/OsoeInternode.cmake) - and
  # `ctest -L internode` runs what the single-node build lacks but its
  # configure check. Labelled `env` alone, it was a second exception, which the
  # README did not name, from 2026-10-12 (B7) to 2026-10-17.
  osoe_internode_label(env.probe.zero-copy-line)
endif()
