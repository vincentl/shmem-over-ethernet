#!/bin/sh
# Run inside the VM. Builds bench.c against the private RelWithDebInfo build.
T=/Users/vincent/shmem-over-ethernet/build/scratch/review-performance-scale/tree
cc -O2 -g -D_GNU_SOURCE -I$T/shmem/include -o $T/build/bench /Users/vincent/shmem-over-ethernet/docs/reviews/evidence/phase1-step2-slice1/performance-scale/bench.c -L$T/build/release/shmem -losoe -Wl,-rpath,$T/build/release/shmem -lpthread
