#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Run inside the VM. Builds bench.c against the private RelWithDebInfo build.
T=<repo>/build/scratch/review-performance-scale/tree
cc -O2 -g -D_GNU_SOURCE -I$T/shmem/include -o $T/build/bench <repo>/docs/reviews/evidence/phase1-step2-slice1/performance-scale/bench.c -L$T/build/release/shmem -losoe -Wl,-rpath,$T/build/release/shmem -lpthread
