add_executable(osoe-launch launch.c)
osoe_warnings(osoe-launch)

# Self-tests: the launcher sets the bootstrap variables for every rank, fails on
# a nonzero exit, and fails on timeout.
add_test(NAME launch.env
  COMMAND osoe-launch -n 4 -t 20 -e 2:OSOE_EXTRA=x -- sh -c
    "test \"$OSOE_PROCID\" = \"$OSOE_LOCALID\" && test \"$OSOE_STEP_NUM_TASKS\" = 4 && test \"$OSOE_GTIDS\" = 0,1,2,3 && test \"$OSOE_NODEID\" = 0 && test -n \"$OSOE_JOB_ID\" && { test \"$OSOE_PROCID\" != 2 || test \"$OSOE_EXTRA\" = x; }")
add_test(NAME launch.failure COMMAND osoe-launch -n 3 -t 20 -- sh -c "test \"$OSOE_PROCID\" != 1")
add_test(NAME launch.timeout COMMAND osoe-launch -n 2 -t 1 -- sleep 30)
set_tests_properties(launch.env launch.failure launch.timeout PROPERTIES
  LABELS "tier2" TIMEOUT 30)
set_tests_properties(launch.failure launch.timeout PROPERTIES WILL_FAIL TRUE)
