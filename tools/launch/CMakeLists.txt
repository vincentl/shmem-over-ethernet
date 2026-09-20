add_executable(osoe-launch launch.c)
target_link_libraries(osoe-launch PRIVATE osoe_base)
osoe_warnings(osoe-launch)

# Self-tests: the launcher sets the bootstrap variables for every rank, fails on
# a nonzero exit, and fails on timeout.
add_test(NAME launch.env
  COMMAND osoe-launch -n 4 -t 20 -e 2:OSOE_EXTRA=x -- sh -c
    "test \"$OSOE_PROCID\" = \"$OSOE_LOCALID\" && test \"$OSOE_STEP_NUM_TASKS\" = 4 && test \"$OSOE_GTIDS\" = 0,1,2,3 && test \"$OSOE_NODEID\" = 0 && test -n \"$OSOE_JOB_ID\" && { test \"$OSOE_PROCID\" != 2 || test \"$OSOE_EXTRA\" = x; }")
# §8.1's node map, derived into the per-rank variables Slurm would set. The
# cyclic pole 0,1,2,0,1,0 is the one that catches an implementation assuming a
# node's ranks are contiguous: rank 3 must be local id 1 of node 0, whose gtids
# are 0,3,5. A launcher that ignored -m would make it local id 3 of node 0.
add_test(NAME launch.node-map
  COMMAND osoe-launch -n 6 -m 0,1,2,0,1,0 -t 20 -- sh -c
    "case \"$OSOE_PROCID\" in \
       0) test \"$OSOE_NODEID\" = 0 && test \"$OSOE_LOCALID\" = 0 && test \"$OSOE_GTIDS\" = 0,3,5 ;; \
       1) test \"$OSOE_NODEID\" = 1 && test \"$OSOE_LOCALID\" = 0 && test \"$OSOE_GTIDS\" = 1,4 ;; \
       2) test \"$OSOE_NODEID\" = 2 && test \"$OSOE_LOCALID\" = 0 && test \"$OSOE_GTIDS\" = 2 ;; \
       3) test \"$OSOE_NODEID\" = 0 && test \"$OSOE_LOCALID\" = 1 && test \"$OSOE_GTIDS\" = 0,3,5 ;; \
       4) test \"$OSOE_NODEID\" = 1 && test \"$OSOE_LOCALID\" = 1 && test \"$OSOE_GTIDS\" = 1,4 ;; \
       5) test \"$OSOE_NODEID\" = 0 && test \"$OSOE_LOCALID\" = 2 && test \"$OSOE_GTIDS\" = 0,3,5 ;; \
       *) false ;; \
     esac")

# The map may equally arrive in the launcher's own environment (§8.1).
add_test(NAME launch.node-map-env
  COMMAND ${CMAKE_COMMAND} -E env OSOE_NODE_MAP=0,0,1 $<TARGET_FILE:osoe-launch>
          -n 3 -t 20 -- sh -c
    "test \"$OSOE_PROCID\" != 2 || { test \"$OSOE_NODEID\" = 1 && test \"$OSOE_GTIDS\" = 2; }")

# A map that leaves a logical node with no PEs is refused, not renumbered.
add_test(NAME launch.node-map-gap COMMAND osoe-launch -n 4 -m 0,0,2,2 -t 20 -- true)

add_test(NAME launch.failure COMMAND osoe-launch -n 3 -t 20 -- sh -c "test \"$OSOE_PROCID\" != 1")
add_test(NAME launch.timeout COMMAND osoe-launch -n 2 -t 1 -- sleep 30)
set_tests_properties(launch.env launch.node-map launch.node-map-env launch.node-map-gap
  launch.failure launch.timeout PROPERTIES LABELS "tier2" TIMEOUT 30)
set_tests_properties(launch.node-map-gap launch.failure launch.timeout PROPERTIES
  WILL_FAIL TRUE)
