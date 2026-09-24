# osoe-bench: what the library costs on a cluster, one measurement a line (the
# Pi runbook, docs/pi-cluster.md). Not installed. Its smoke test runs every
# measurement once, quickly, on two logical nodes of two PEs; the numbers it
# prints on loopback are not the point.
add_executable(osoe-bench osoe-bench.c)
target_link_libraries(osoe-bench PRIVATE osoe)
osoe_warnings(osoe-bench)
add_test(NAME tools.bench.0-0-1-1
  COMMAND osoe-launch -n 4 -m 0,0,1,1 -t 240 -- $<TARGET_FILE:osoe-bench> quick)
set_tests_properties(tools.bench.0-0-1-1 PROPERTIES LABELS "tier2" TIMEOUT 300
  PASS_REGULAR_EXPRESSION "pairs pes=4 total_mib_per_s=")
