#!/bin/sh
# Run inside the VM. T = private copy of the tree with a release build.
# Links the library's own objects because the internal symbols are hidden in libosoe.so.
T=/Users/vincent/shmem-over-ethernet/build/scratch/review-performance-scale/tree
cc -O1 -D_GNU_SOURCE -I$T -I$T/shmem/src -I$T/shmem/include -o $T/build/struct-sizes /Users/vincent/shmem-over-ethernet/docs/reviews/evidence/phase1-step2-slice1/performance-scale/struct-sizes.c $(find $T/build/release/shmem/CMakeFiles/osoe.dir -name '*.o') $T/build/release/base/libosoe_base.a -lpthread 2>&1 || exit 1
$T/build/struct-sizes
