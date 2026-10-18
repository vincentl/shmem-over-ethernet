# SPDX-License-Identifier: LGPL-3.0-or-later
find_package(Threads REQUIRED)

foreach(name size environment node-map futex epoch stall-watch hostlist siphash lock-free
             print-line)
  add_executable(base_test_${name} test-${name}.c)
  target_link_libraries(base_test_${name} PRIVATE osoe_base Threads::Threads)
  osoe_warnings(base_test_${name})
  add_test(NAME base.${name} COMMAND base_test_${name})
  set_tests_properties(base.${name} PROPERTIES LABELS "tier1" TIMEOUT 60)
endforeach()

# The shared atomics lock-free (base/lock-free.h; §3.16; the step D review,
# portability Q4), in the two answers no build here meets: base.lock-free, in
# the loop above, has long's atomics sometimes lock-free and asks the objects,
# as shmem_init does at start-up; this one has them never lock-free, and the
# compile must be refused by the header's assertion.
add_test(NAME base.lock-free.never
  COMMAND ${CMAKE_C_COMPILER} -std=c11 -fsyntax-only -DOSOE_TEST_LOCK_FREE_NEVER
          -I${PROJECT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/test-lock-free.c)
set_tests_properties(base.lock-free.never PROPERTIES LABELS "tier1" TIMEOUT 30
  PASS_REGULAR_EXPRESSION "must be lock-free")

# The serializing fence orders a store before a later load (§3.16, §4.6). Not
# under ThreadSanitizer, which would report the race it measures; skipped (77)
# when its control cannot see a reordering at all.
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  add_executable(base_test_serializing-fence test-serializing-fence.c)
  target_link_libraries(base_test_serializing-fence PRIVATE osoe_base Threads::Threads)
  osoe_warnings(base_test_serializing-fence)
  add_test(NAME base.serializing-fence COMMAND base_test_serializing-fence)
  set_tests_properties(base.serializing-fence PROPERTIES LABELS "tier1" TIMEOUT 60
    SKIP_RETURN_CODE 77)
endif()
