add_executable(shmem_test_info test_info.c)
target_link_libraries(shmem_test_info PRIVATE osoe)
osoe_warnings(shmem_test_info)
add_test(NAME shmem.info COMMAND shmem_test_info)
set_tests_properties(shmem.info PROPERTIES LABELS "tier1" TIMEOUT 30)

if(BUILD_SHARED_LIBS)
  add_test(NAME shmem.exports
    COMMAND ${CMAKE_COMMAND} -DLIB=$<TARGET_FILE:osoe>
            -P ${PROJECT_SOURCE_DIR}/cmake/CheckExports.cmake)
  set_tests_properties(shmem.exports PROPERTIES LABELS "tier1" TIMEOUT 30)
endif()

# The allocator on its own (tier 1): compiled in, since libosoe hides it.
add_executable(shmem_test_alloc test_alloc.c ../src/alloc.c)
target_include_directories(shmem_test_alloc PRIVATE ../src)
osoe_warnings(shmem_test_alloc)
add_test(NAME shmem.alloc COMMAND shmem_test_alloc)
set_tests_properties(shmem.alloc PROPERTIES LABELS "tier1" TIMEOUT 60)

# The §4.9 watched-range encoding on its own (tier 1): pure logic, and the
# property it must have is one a brute-force check can state directly.
add_executable(shmem_test_watchrange test_watchrange.c)
target_include_directories(shmem_test_watchrange PRIVATE ../src)
osoe_warnings(shmem_test_watchrange)
add_test(NAME shmem.watchrange COMMAND shmem_test_watchrange)
set_tests_properties(shmem.watchrange PROPERTIES LABELS "tier1" TIMEOUT 60)

# Multi-process tests (tier 2): each program runs under osoe-launch at every
# listed local PE count, as shmem.<name>.L<count>.
set(OSOE_LAUNCH_COUNTS 1 2 4)

# Every tier-2 test asserts §8.2's rule that invariant counters are zero at
# quiescence (invariants.h). It reads the control segment through osoe-stat's
# segment reader, compiled in: shared with the tool, deliberately not with the
# library, since a check that used the library's own headers would only prove
# the library agrees with itself.
function(osoe_tier2_test name)
  add_executable(shmem_test_${name} test_${name}.c
    ${PROJECT_SOURCE_DIR}/tools/stat/segment.c)
  target_include_directories(shmem_test_${name} PRIVATE
    ${PROJECT_SOURCE_DIR}/tools/stat)
  target_link_libraries(shmem_test_${name} PRIVATE osoe rt)
  osoe_warnings(shmem_test_${name})
  foreach(count IN LISTS OSOE_LAUNCH_COUNTS)
    add_test(NAME shmem.${name}.L${count}
      COMMAND osoe-launch -n ${count} -t 60 -- $<TARGET_FILE:shmem_test_${name}>)
    set_tests_properties(shmem.${name}.L${count} PROPERTIES LABELS "tier2" TIMEOUT 90)
  endforeach()
endfunction()

osoe_tier2_test(init)
osoe_tier2_test(fork)
osoe_tier2_test(barrier)
osoe_tier2_test(malloc)
osoe_tier2_test(rma)
osoe_tier2_test(amo)
osoe_tier2_test(wait)
osoe_tier2_test(signal)
osoe_tier2_test(quiet)
osoe_tier2_test(heap)

# osoe-stat, against a live job and against one deliberately stalled (§3.14).
# It needs the tool's path, which only the build knows; the launcher passes its
# environment through, so setting it on the test reaches every rank.
osoe_tier2_test(stat)
foreach(count IN LISTS OSOE_LAUNCH_COUNTS)
  set_tests_properties(shmem.stat.L${count} PROPERTIES
    ENVIRONMENT "OSOE_TEST_STAT=$<TARGET_FILE:osoe-stat>")
endforeach()

# The development watchdog (§3.15). Not an osoe_tier2_test: it needs -k, so the
# launcher leaves the survivors to the library, and it asserts on output rather
# than on an exit status. A PASS_REGULAR_EXPRESSION test ignores the exit code,
# which is what these need: the job is meant to fail.
add_executable(shmem_test_watchdog test_watchdog.c)
target_link_libraries(shmem_test_watchdog PRIVATE osoe)
osoe_warnings(shmem_test_watchdog)

foreach(count 2 4)
  foreach(where barrier wait_until)
    add_test(NAME shmem.watchdog.${where}.L${count}
      COMMAND osoe-launch -n ${count} -t 20 -k
              -e 0:OSOE_TEST_WAIT=${where} -e 1:OSOE_TEST_WAIT=${where}
              -e 2:OSOE_TEST_WAIT=${where} -e 3:OSOE_TEST_WAIT=${where}
              -e 0:OSOE_WATCHDOG=1 -e 1:OSOE_WATCHDOG=1
              -e 2:OSOE_WATCHDOG=1 -e 3:OSOE_WATCHDOG=1
              -- $<TARGET_FILE:shmem_test_watchdog>)
    set_tests_properties(shmem.watchdog.${where}.L${count} PROPERTIES
      LABELS "tier2" TIMEOUT 60
      PASS_REGULAR_EXPRESSION "has died; this PE would wait for it forever")
  endforeach()
endforeach()

# Turned off, the same job hangs until the launcher's timeout: this is what the
# watchdog replaces, kept as a test so the difference stays demonstrated.
add_test(NAME shmem.watchdog.off
  COMMAND osoe-launch -n 2 -t 3 -k -e 0:OSOE_WATCHDOG=0 -e 1:OSOE_WATCHDOG=0
          -- $<TARGET_FILE:shmem_test_watchdog>)
set_tests_properties(shmem.watchdog.off PROPERTIES
  LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "osoe-launch: timeout after 3 s")

# With nothing set, the default must follow the preset: on in debug, asan and
# tsan, off in release (§3.15).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(osoe_watchdog_default_expect "has died; this PE would wait for it forever")
else()
  set(osoe_watchdog_default_expect "osoe-launch: timeout after 3 s")
endif()
add_test(NAME shmem.watchdog.default
  COMMAND osoe-launch -n 2 -t 3 -k -- $<TARGET_FILE:shmem_test_watchdog>)
set_tests_properties(shmem.watchdog.default PROPERTIES
  LABELS "tier2" TIMEOUT 60
  PASS_REGULAR_EXPRESSION "${osoe_watchdog_default_expect}")

# Init must refuse what it cannot run, saying why.
add_test(NAME shmem.refuse.no_launcher
  COMMAND ${CMAKE_COMMAND} -E env --unset=OSOE_PROCID --unset=SLURM_JOB_ID
          $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.no_launcher PROPERTIES
  PASS_REGULAR_EXPRESSION "no launcher environment")
add_test(NAME shmem.refuse.multinode
  COMMAND ${CMAKE_COMMAND} -E env OSOE_PROCID=0 OSOE_LOCALID=0 OSOE_STEP_NUM_TASKS=2
          OSOE_NODEID=0 OSOE_GTIDS=0 OSOE_JOB_ID=1 OSOE_STEP_ID=0
          $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.multinode PROPERTIES
  PASS_REGULAR_EXPRESSION "multi-node jobs are not yet supported")
add_test(NAME shmem.refuse.bad_size
  COMMAND osoe-launch -n 2 -t 60 -e 0:SHMEM_SYMMETRIC_SIZE=12q -e 1:SHMEM_SYMMETRIC_SIZE=12q
          -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.bad_size PROPERTIES
  PASS_REGULAR_EXPRESSION "SHMEM_SYMMETRIC_SIZE='12q' is invalid")
add_test(NAME shmem.refuse.size_mismatch
  COMMAND osoe-launch -n 3 -t 60 -e 2:SHMEM_SYMMETRIC_SIZE=2m
          -- $<TARGET_FILE:shmem_test_init>)
set_tests_properties(shmem.refuse.size_mismatch PROPERTIES
  PASS_REGULAR_EXPRESSION "SHMEM_SYMMETRIC_SIZE differs between PEs")
add_executable(shmem_test_badput test_badput.c)
target_link_libraries(shmem_test_badput PRIVATE osoe)
osoe_warnings(shmem_test_badput)
add_test(NAME shmem.refuse.nonsymmetric_put
  COMMAND osoe-launch -n 1 -t 60 -- $<TARGET_FILE:shmem_test_badput>)
set_tests_properties(shmem.refuse.nonsymmetric_put PROPERTIES
  PASS_REGULAR_EXPRESSION "shmem_putmem: .* is not within the symmetric heap")
set_tests_properties(shmem.refuse.no_launcher shmem.refuse.multinode
  shmem.refuse.bad_size shmem.refuse.size_mismatch shmem.refuse.nonsymmetric_put PROPERTIES
  LABELS "tier2" TIMEOUT 90)
