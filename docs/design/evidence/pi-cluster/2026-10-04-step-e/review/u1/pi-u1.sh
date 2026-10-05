#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round six's U1 on the Pi: piece-edge (reductions and broadcasts either side of
# 16 KiB multiples) with main before the fix (~/osoe, 375973a, built) against the
# fix (TAR, built under ~/osoe/build/compare/u1), alternating, three runs a side,
# at 4 x 4 and 4 x 1. Usage: pi-u1.sh TAR (from the Mac).
set -u
TAR=$1
D=<repo>/build/scratch/review-e/u1
OUT=$D/out; mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
scp -q <repo>/build/scratch/review-e/pi/piece-edge.c pi-login:osoe/build/compare/piece-edge.c || exit 1
scp -q "$TAR" pi-login:osoe/build/compare/u1.tar || exit 1
ssh pi-login 'cd osoe/build/compare && rm -rf u1 && mkdir u1 && tar -C u1 -xf u1.tar && cd u1 && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../u1-build.txt 2>&1; echo "u1 built: $?"
  cd ~/osoe && build/release/tools/wrapper/oshcc -O2 -o build/compare/edge-before build/compare/piece-edge.c; echo "before probe: $?"
  build/compare/u1/build/release/tools/wrapper/oshcc -O2 -o build/compare/edge-after build/compare/piece-edge.c; echo "after probe: $?"' >> $LOG
for i in 1 2 3; do
  for side in before after; do
    ssh pi-login "cd osoe && srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 build/compare/edge-$side > build/compare/u1-$side-4x4-$i.txt 2>&1; echo \"$side 4x4 $i: \$?\"; srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 build/compare/edge-$side > build/compare/u1-$side-4x1-$i.txt 2>&1; echo \"$side 4x1 $i: \$?\"" >> $LOG
  done
done
scp -q 'pi-login:osoe/build/compare/u1-*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
