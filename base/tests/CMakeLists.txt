# SPDX-License-Identifier: LGPL-3.0-or-later
find_package(Threads REQUIRED)

foreach(name size environment node-map futex epoch stall-watch hostlist siphash)
  add_executable(base_test_${name} test-${name}.c)
  target_link_libraries(base_test_${name} PRIVATE osoe_base Threads::Threads)
  osoe_warnings(base_test_${name})
  add_test(NAME base.${name} COMMAND base_test_${name})
  set_tests_properties(base.${name} PROPERTIES LABELS "tier1" TIMEOUT 60)
endforeach()

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
