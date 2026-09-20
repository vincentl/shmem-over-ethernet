#!/bin/sh
# Run inside the VM from the private tree: build/scratch/review-protocols/tree
# (a copy of the repository with ack-echo.patch applied and the debug preset built).
set -e
T=/Users/vincent/shmem-over-ethernet/build/scratch/review-protocols/tree
E=/Users/vincent/shmem-over-ethernet/docs/reviews/evidence/phase1-step2-slice1/protocols
cc -O1 -I$T/shmem/include -o $T/build/ack-echo $E/ack-echo.c -L$T/build/debug/shmem -losoe -Wl,-rpath,$T/build/debug/shmem
for quiet in 0 1; do
  for ms in 500 2000; do
    echo "== one put + quiet: $quiet, then ${ms} ms of application sleep"
    OSOE_REVIEW_PRINT=1 OSOE_ECHO_QUIET=$quiet OSOE_ECHO_SLEEP_MS=$ms \
      $T/build/debug/tools/launch/osoe-launch -n 2 -m 0,1 -t 60 -- $T/build/ack-echo 2>&1 | grep review: | sort
  done
done
