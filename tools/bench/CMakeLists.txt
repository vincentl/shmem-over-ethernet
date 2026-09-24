# osoe-bench: what the library costs on a cluster, one measurement a line (the
# Pi runbook, docs/pi-cluster.md). Not installed. Its smoke test runs every
# measurement once, quickly, on two logical nodes of two PEs; the numbers it
# prints on loopback are not the point.
add_executable(osoe-bench osoe-bench.c)
target_link_libraries(osoe-bench PRIVATE osoe)
osoe_warnings(osoe-bench)
# Not under ThreadSanitizer, like every test over a node map: a remote AMO's
# write is ordered after the target's own earlier writes through another
# process - a barrier across nodes - which it cannot see, and it reported the
# agent's apply racing the calloc that zeroed the word (the verification of
# phase 1 step 6 slice 1).
if(NOT CMAKE_C_FLAGS MATCHES "sanitize=thread")
  add_test(NAME tools.bench.0-0-1-1
    COMMAND osoe-launch -n 4 -m 0,0,1,1 -t 240 -- $<TARGET_FILE:osoe-bench> quick)
  set_tests_properties(tools.bench.0-0-1-1 PROPERTIES LABELS "tier2" TIMEOUT 300
    PASS_REGULAR_EXPRESSION "pairs pes=4 total_mib_per_s="
    FAIL_REGULAR_EXPRESSION "exited with status [1-9]|killed by signal|timeout after|left behind")
endif()
