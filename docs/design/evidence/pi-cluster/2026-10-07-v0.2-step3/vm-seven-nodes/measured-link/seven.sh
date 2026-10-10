#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 3 Part 3, the VM's half: e-band on seven logical nodes of one PE,
# `link` against `tree`, alternating three times, with SHMEM_DEBUG on so that
# each run records the link it measured; and the shapes chosen at 16 and 64
# logical nodes (shmem.shape.chosen.*, shmem.circulant.16/64). Usage: seven.sh
# TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; D=$2; mkdir -p $D; OUT=$D/seven.txt; : > $OUT
T=/tmp/v02-circ-seven-$$; rm -rf $T; mkdir -p $T; tar -C $T -xf $TAR; cd $T
cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > $D/build.txt 2>&1 ||
  { echo "build failed" >> $OUT; exit 1; }
O=$T/build/release/tools/wrapper/oshcc
L=$T/build/release/tools/launch/osoe-launch
$O -O2 -o $T/e-band <repo>/docs/design/evidence/pi-cluster/2026-10-04-step-e/e-band.c || { echo "e-band build failed" >> $OUT; exit 1; }
for i in 1 2 3; do
  for side in link tree; do
    echo "== $side $i $(date +%H:%M:%S)" >> $OUT
    SHMEM_DEBUG=1 OSOE_COLLECTIVE_SHAPE=$side $L -n 7 -m 0,1,2,3,4,5,6 -t 600 -- $T/e-band > $D/band-7-$side-$i.txt 2>&1
    echo "exit $?" >> $OUT
  done
done
echo "== shapes at 16 and 64 $(date +%H:%M:%S)" >> $OUT
ctest --test-dir build/release -R 'shmem\.shape\.chosen\.(pi|loopback)\.(16|64)$|shmem\.circulant\.(16|64)$' --output-on-failure > $D/shapes.txt 2>&1
grep -E "Test +#|tests passed" $D/shapes.txt >> $OUT
cd /; rm -rf $T
echo "DONE $(date +%H:%M:%S)" >> $OUT
