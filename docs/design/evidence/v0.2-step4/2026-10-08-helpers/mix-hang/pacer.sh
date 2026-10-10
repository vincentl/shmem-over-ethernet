#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4 part 3: osoe-pacer's mix and mailbox for a minute each, at no
# helpers and at two, 8 PEs on four logical nodes, from a release build.
# usage: pacer.sh TAR OUTDIR. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; D=$2; mkdir -p $D; OUT=$D/pacer.txt; : > $OUT
T=/tmp/v02-helpers-pacer-$$; rm -rf $T; mkdir -p $T; tar -C $T -xf $TAR; cd $T
cmake --preset release > $D/configure.txt 2>&1
if ! cmake --build build/release -j4 > $D/build.txt 2>&1; then
  echo "BUILD FAILED" >> $OUT; cd /; rm -rf $T; echo "DONE" >> $OUT; exit 1
fi
L=build/release/tools/launch/osoe-launch; P=build/release/tools/pacer/osoe-pacer
for H in 0 2; do
  for ex in mix mailbox; do
    echo "== $ex at $H helpers $(date +%H:%M:%S)" >> $OUT
    OSOE_PROGRESS_HELPERS=$H $L -n 8 -m 0,0,1,1,2,2,3,3 -t 180 -- $P --seconds 60 $ex > $D/$ex-$H.txt 2>&1
    echo "launcher exited $?" >> $OUT
    grep -E "osoe-pacer failures=|FAIL|osoe: PE|timeout after|killed by" $D/$ex-$H.txt | head -8 >> $OUT
  done
done
cd /; rm -rf $T
echo "DONE $(date +%H:%M:%S)" >> $OUT
