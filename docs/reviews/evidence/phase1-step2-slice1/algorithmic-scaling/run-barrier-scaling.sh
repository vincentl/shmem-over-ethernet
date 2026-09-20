#!/bin/sh
# Review evidence: build and run barrier-scaling.c against a PRIVATE release
# build of the tree (never the main build directories). Run inside the VM.
# Results are "measured under contention": 4 cores, shared with other agents.
set -e
ROOT=/Users/vincent/shmem-over-ethernet
T=$ROOT/build/scratch/review-algorithmic-scaling/tree
B=$T/build/release
HERE=$ROOT/docs/reviews/evidence/phase1-step2-slice1/algorithmic-scaling
cc -O2 -I$T/shmem/include -o $B/barrier-scaling $HERE/barrier-scaling.c \
   -L$B/shmem -losoe -Wl,-rpath,$B/shmem
export OSOE_AM_CHUNK=1024 OSOE_AM_BUFFER=2048
echo "# internode star: one PE per logical node"
for n in 2 3 4 6 8 12 16; do
  map=$(seq -s, 0 $((n-1)))
  echo "map nodes=$n L=1: $($B/tools/launch/osoe-launch -n $n -m $map -t 120 -- $B/barrier-scaling 1000)"
done
echo "# intranode: one logical node, L PEs"
for n in 1 2 4 8 16 32; do
  map=$(yes 0 | head -$n | paste -sd,)
  echo "map nodes=1 L=$n: $($B/tools/launch/osoe-launch -n $n -m $map -t 120 -- $B/barrier-scaling 1000)"
done
