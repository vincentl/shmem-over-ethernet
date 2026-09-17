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

# Multi-process tests (tier 2): each program runs under osoe-launch at every
# listed local PE count, as shmem.<name>.L<count>.
set(OSOE_LAUNCH_COUNTS 1 2 4)

function(osoe_tier2_test name)
  add_executable(shmem_test_${name} test_${name}.c)
  target_link_libraries(shmem_test_${name} PRIVATE osoe)
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
set_tests_properties(shmem.refuse.no_launcher shmem.refuse.multinode
  shmem.refuse.bad_size shmem.refuse.size_mismatch PROPERTIES
  LABELS "tier2" TIMEOUT 90)
