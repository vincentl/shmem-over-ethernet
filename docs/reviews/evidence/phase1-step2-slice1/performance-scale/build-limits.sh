#!/bin/sh
T=/Users/vincent/shmem-over-ethernet/build/scratch/review-performance-scale/tree
for p in limit-pool-exhaustion limit-reply-starvation; do
cc -O2 -g -I$T/shmem/include -o $T/build/$p /Users/vincent/shmem-over-ethernet/docs/reviews/evidence/phase1-step2-slice1/performance-scale/$p.c -L$T/build/release/shmem -losoe -Wl,-rpath,$T/build/release/shmem -lpthread || exit 1
done
