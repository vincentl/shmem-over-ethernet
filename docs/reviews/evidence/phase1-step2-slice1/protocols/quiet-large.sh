#!/bin/sh
# Run inside the VM. $1 = tree (tree | tree-naive) under build/scratch/review-protocols.
T=<repo>/build/scratch/review-protocols/$1
E=<repo>/docs/reviews/evidence/phase1-step2-slice1/protocols
cc -O1 -I$T/shmem/include -o $T/build/quiet-large $E/quiet-large.c -L$T/build/debug/shmem -losoe -Wl,-rpath,$T/build/debug/shmem || exit 1
for run in 1 2 3; do
  echo "== $1 run $run"
  OSOE_REVIEW_PRINT=1 OSOE_BOOTSTRAP_TIMEOUT=10 $T/build/debug/tools/launch/osoe-launch -n 2 -m 0,1 -t 60 -- $T/build/quiet-large 2>&1 | tail -4
done
